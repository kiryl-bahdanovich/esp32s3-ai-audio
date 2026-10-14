#include "openai_http.h"

#include "openai_tls.h"

#include <WiFi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace openai_http
{

namespace
{

bool waitAvailable(WiFiClientSecure &client, uint32_t timeoutMs)
{
    const uint32_t start = millis();
    while (!client.available())
    {
        if (!client.connected() || millis() - start >= timeoutMs)
        {
            return false;
        }
        delay(1);
    }
    return true;
}

bool readExact(WiFiClientSecure &client, char *buf, size_t len, uint32_t timeoutMs)
{
    size_t got = 0;
    const uint32_t start = millis();
    while (got < len)
    {
        if (!client.available())
        {
            if (!client.connected() || millis() - start >= timeoutMs)
            {
                return false;
            }
            delay(1);
            continue;
        }
        const int c = client.read();
        if (c < 0)
        {
            return false;
        }
        buf[got++] = static_cast<char>(c);
    }
    return true;
}

/**
 * Reads exactly len bytes into out via a stack buffer + concat(), instead of
 * one client.read()/String::operator+= per byte. Cuts both the number of
 * TLS-layer read() calls and the number of String reallocations for bodies
 * of any real size (chunked Responses payloads can run into the KBs).
 */
bool readBulk(WiFiClientSecure &client, String &out, size_t len, uint32_t timeoutMs)
{
    uint8_t buf[512];
    size_t remaining = len;
    while (remaining > 0)
    {
        if (!waitAvailable(client, timeoutMs))
        {
            return false;
        }
        const size_t want = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        const int got = client.read(buf, want);
        if (got <= 0)
        {
            if (!client.connected())
            {
                return false;
            }
            delay(1);
            continue;
        }
        out.concat(reinterpret_cast<const char *>(buf), static_cast<unsigned int>(got));
        remaining -= static_cast<size_t>(got);
    }
    return true;
}

String readLine(WiFiClientSecure &client, uint32_t timeoutMs)
{
    String line;
    const uint32_t start = millis();
    while (millis() - start < timeoutMs)
    {
        if (!client.available())
        {
            if (!client.connected())
            {
                break;
            }
            delay(1);
            continue;
        }
        const int c = client.read();
        if (c < 0)
        {
            break;
        }
        if (c == '\n')
        {
            break;
        }
        if (c != '\r')
        {
            line += static_cast<char>(c);
        }
    }
    return line;
}

} // namespace

String jsonEscape(const char *s)
{
    String out;
    out.reserve(strlen(s) + 8);
    for (const char *p = s; *p; ++p)
    {
        const char c = *p;
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

bool writeAll(WiFiClientSecure &client, const uint8_t *data, size_t len)
{
    size_t written = 0;
    while (written < len)
    {
        if (!client.connected())
        {
            return false;
        }
        const size_t n = client.write(data + written, len - written);
        if (n == 0)
        {
            delay(1);
            continue;
        }
        written += n;
    }
    return true;
}

bool writeAll(WiFiClientSecure &client, const char *data, size_t len)
{
    return writeAll(client, reinterpret_cast<const uint8_t *>(data), len);
}

bool readHttpResponse(WiFiClientSecure &client, HttpResponse &out, uint32_t timeoutMs)
{
    if (!waitAvailable(client, timeoutMs))
    {
        return false;
    }

    const String statusLine = readLine(client, timeoutMs);
    {
        const int sp1 = statusLine.indexOf(' ');
        out.status = (sp1 >= 0) ? statusLine.substring(sp1 + 1).toInt() : -1;
    }

    out.chunked = false;
    out.contentLength = -1;

    for (;;)
    {
        const String line = readLine(client, timeoutMs);
        if (line.length() == 0)
        {
            break;
        }

        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0)
        {
            out.chunked = true;
        }
        else if (lower.startsWith("content-length:"))
        {
            out.contentLength = line.substring(line.indexOf(':') + 1).toInt();
        }
    }

    out.body = "";
    out.body.reserve(out.contentLength > 0 ? static_cast<unsigned>(out.contentLength) : 2048);

    if (out.chunked)
    {
        for (;;)
        {
            String sizeLine = readLine(client, timeoutMs);
            sizeLine.trim();
            const int semi = sizeLine.indexOf(';');
            if (semi >= 0)
            {
                sizeLine = sizeLine.substring(0, semi);
            }
            sizeLine.trim();
            if (sizeLine.length() == 0)
            {
                continue;
            }

            char *endPtr = nullptr;
            const unsigned long chunkSize = strtoul(sizeLine.c_str(), &endPtr, 16);
            if (chunkSize == 0)
            {
                for (;;)
                {
                    const String trailer = readLine(client, timeoutMs);
                    if (trailer.length() == 0)
                    {
                        break;
                    }
                }
                break;
            }

            if (!readBulk(client, out.body, static_cast<size_t>(chunkSize), timeoutMs))
            {
                return false;
            }

            char crlf[2];
            if (!readExact(client, crlf, 2, timeoutMs))
            {
                return false;
            }
        }
    }
    else if (out.contentLength >= 0)
    {
        if (!readBulk(client, out.body, static_cast<size_t>(out.contentLength), timeoutMs))
        {
            return false;
        }
    }
    else
    {
        uint8_t buf[512];
        const uint32_t start = millis();
        while (millis() - start < timeoutMs)
        {
            while (client.available())
            {
                const int avail = client.available();
                const size_t want = (static_cast<size_t>(avail) < sizeof(buf)) ? static_cast<size_t>(avail) : sizeof(buf);
                const int got = client.read(buf, want);
                if (got > 0)
                {
                    out.body.concat(reinterpret_cast<const char *>(buf), static_cast<unsigned int>(got));
                }
            }
            if (!client.connected() && !client.available())
            {
                break;
            }
            delay(5);
        }
    }

    return true;
}

bool postJson(
    const char *apiKey,
    const char *path,
    const String &body,
    uint32_t timeoutSec,
    HttpResponse &out)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return false;
    }

    // Reused across calls so repeat requests to api.openai.com skip the TLS
    // handshake (which otherwise costs several hundred ms or more per call).
    // "Connection: keep-alive" below asks the server to leave the socket
    // open; if it (or an idle timeout) closed it anyway, the reconnect-and-
    // retry logic further down transparently opens a fresh connection.
    static WiFiClientSecure client;
    static bool clientConfigured = false;
    if (!clientConfigured)
    {
        openai_tls::configureClient(client);
        clientConfigured = true;
    }
    client.setTimeout(timeoutSec);

    String headers;
    headers.reserve(256 + strlen(apiKey));
    headers += "POST ";
    headers += path;
    headers += " HTTP/1.1\r\n";
    headers += "Host: ";
    headers += kHost;
    headers += "\r\n";
    headers += "Authorization: Bearer ";
    headers += apiKey;
    headers += "\r\n";
    headers += "Content-Type: application/json\r\n";
    headers += "Accept: application/json\r\n";
    headers += "Accept-Encoding: identity\r\n";
    headers += "Content-Length: ";
    headers += String(body.length());
    headers += "\r\n";
    headers += "Connection: keep-alive\r\n\r\n";

    bool sent = client.connected() &&
                writeAll(client, headers.c_str(), headers.length()) &&
                writeAll(client, body.c_str(), body.length());

    if (!sent)
    {
        // Either not connected yet, or the kept-alive socket was stale
        // (closed by the server/idle timeout) -- (re)connect once and retry.
        client.stop();
        sent = client.connect(kHost, kPort) &&
               writeAll(client, headers.c_str(), headers.length()) &&
               writeAll(client, body.c_str(), body.length());
    }

    if (!sent)
    {
        client.stop();
        return false;
    }

    const bool ok = readHttpResponse(client, out, timeoutSec * 1000UL);
    if (!ok || out.status != 200)
    {
        // Don't risk reusing a socket that may be in a bad/half-read state
        // for the next request.
        client.stop();
    }
    return ok;
}

} // namespace openai_http
