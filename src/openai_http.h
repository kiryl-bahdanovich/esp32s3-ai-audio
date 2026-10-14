#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <cstddef>
#include <cstdint>

/** Shared HTTPS helpers for api.openai.com (chunked bodies, JSON posts). */
namespace openai_http
{

constexpr const char *kHost = "api.openai.com";
constexpr uint16_t kPort = 443;

String jsonEscape(const char *s);

bool writeAll(WiFiClientSecure &client, const uint8_t *data, size_t len);
bool writeAll(WiFiClientSecure &client, const char *data, size_t len);

struct HttpResponse
{
    int status = -1;
    bool chunked = false;
    int contentLength = -1;
    String body;
};

bool readHttpResponse(WiFiClientSecure &client, HttpResponse &out, uint32_t timeoutMs);

/**
 * POST application/json to path with Bearer auth. Body is sent as one contiguous
 * string (keep Responses / function-result payloads bounded).
 *
 * Reuses a single keep-alive HTTPS connection to api.openai.com across calls
 * (skipping the TLS handshake on repeat requests) and transparently
 * reconnects if the kept-alive socket turns out to be closed/stale.
 */
bool postJson(
    const char *apiKey,
    const char *path,
    const String &body,
    uint32_t timeoutSec,
    HttpResponse &out);

} // namespace openai_http
