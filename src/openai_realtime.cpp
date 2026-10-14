#include "openai_realtime.h"

#include "audio_out.h"
#include "openai_http.h"
#include "openai_responses.h"
#include "openai_tls.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <algorithm>
#include <cstring>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/base64.h>

#ifndef WEBSOCKET_OPCODE_TEXT
#define WEBSOCKET_OPCODE_TEXT 0x1
#endif

namespace openai_realtime
{

namespace
{

constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint32_t kSessionTimeoutMs = 180000;
/** How long to wait for audio_out to finish draining a response to the DAC
 *  after response.done, before giving up and returning anyway. */
constexpr uint32_t kDrainTimeoutMs = 30000;
/** Realtime currently rejects input rates below 24 kHz. */
constexpr uint32_t kRealtimeSampleRateHz = 24000;
constexpr size_t kPcmChunkSamples = 2400; // 100 ms at 24 kHz (destination rate)
constexpr size_t kEncodeChunkBytes = 3072; // multiple of 3 for base64; fewer, bigger WS frames
/** Decoded-size scratch buffer for a response.output_audio.delta chunk,
 *  sized generously for the common case so the hot path never allocates.
 *  Chunks that exceed this (observed in practice: the API does not cap
 *  delta size at a small, predictable value) fall back to a one-off PSRAM
 *  allocation in handleServerEvent rather than being silently dropped --
 *  dropping deltas previously caused near-total silence on real hardware. */
constexpr size_t kAudioDeltaDecodeBufBytes = 32768;

const char *kToolName = "get_web_assisted_answer";

struct SessionState
{
    esp_websocket_client_handle_t client = nullptr;
    SemaphoreHandle_t mu = nullptr;

    // Connection-lifetime state: only touched by onWsEvent()/connectSession()/
    // teardownClient(), and deliberately NOT cleared by resetTurnState() so a
    // live session survives across askUtterance() calls.
    bool connected = false;
    bool sessionCreated = false;
    bool sessionUpdated = false;

    // Per-turn state: cleared by resetTurnState() at the start of every
    // askUtterance() call.
    bool fatalError = false;
    String error;

    String rxBuf;
    int rxPayloadLen = 0;
    int rxGot = 0;

    String outputText;
    String callId;
    String callName;
    String callArgs;
    bool haveFunctionCall = false;
    bool responseDone = false;
    bool functionRound = false; // true while waiting after submitting tool output
};

SessionState g;

void lock()
{
    if (g.mu != nullptr)
    {
        xSemaphoreTake(g.mu, portMAX_DELAY);
    }
}

void unlock()
{
    if (g.mu != nullptr)
    {
        xSemaphoreGive(g.mu);
    }
}

constexpr size_t base64EncodedLen(size_t n)
{
    return 4 * ((n + 2) / 3);
}

bool sendText(const String &msg)
{
    if (g.client == nullptr || !esp_websocket_client_is_connected(g.client))
    {
        return false;
    }
    const int n = esp_websocket_client_send_text(
        g.client, msg.c_str(), static_cast<int>(msg.length()), pdMS_TO_TICKS(10000));
    return n == static_cast<int>(msg.length());
}

String buildSessionUpdate(const Config &config, uint32_t sampleRateHz)
{
    // Server VAD disabled: local vad::poll() owns speech onset/end.
    String s = "{";
    s += "\"type\":\"session.update\",";
    s += "\"session\":{";
    s += "\"type\":\"realtime\",";
    s += "\"model\":\"";
    s += config.realtimeModel;
    s += "\",";
    s += "\"output_modalities\":[\"audio\"],";
    s += "\"instructions\":\"";
    s += openai_http::jsonEscape(config.instructions);
    s += "\",";
    s += "\"audio\":{";
    s += "\"input\":{";
    s += "\"format\":{\"type\":\"audio/pcm\",\"rate\":";
    s += String(sampleRateHz);
    s += "},";
    s += "\"turn_detection\":null";
    s += "},";
    s += "\"output\":{";
    s += "\"format\":{\"type\":\"audio/pcm\",\"rate\":";
    s += String(audio_out::kSampleRateHz);
    s += "},";
    s += "\"voice\":\"";
    s += config.voice;
    s += "\"";
    s += "}";
    s += "},";
    s += "\"tools\":[{";
    s += "\"type\":\"function\",";
    s += "\"name\":\"";
    s += kToolName;
    s += "\",";
    s += "\"description\":\"Answer a question that may require current information "
         "from the internet. Call this when the question depends on current events, "
         "recent changes, current prices, recent product specifications, or other "
         "facts you cannot reliably answer from existing knowledge. Do not call this "
         "for stable explanations you already know.\",";
    s += "\"parameters\":{";
    s += "\"type\":\"object\",";
    s += "\"properties\":{";
    s += "\"question\":{";
    s += "\"type\":\"string\",";
    s += "\"description\":\"The user's question to research and answer.\"";
    s += "}";
    s += "},";
    s += "\"required\":[\"question\"]";
    s += "}";
    s += "}],";
    s += "\"tool_choice\":\"auto\"";
    s += "}";
    s += "}";
    return s;
}

void handleServerEvent(const String &json)
{
    // Fast path: text deltas are by far the most frequent event while the
    // model is streaming a response, and only need "type" + "delta". A
    // filtered parse skips allocating/parsing nodes for the rest of the
    // payload on this hot path.
    JsonDocument filter;
    filter["type"] = true;
    filter["delta"] = true;
    filter["error"]["message"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err)
    {
        Serial.printf("[realtime] skip unparseable event (%s)\n", err.c_str());
        return;
    }

    const char *type = doc["type"] | "";
    if (type[0] == '\0')
    {
        return;
    }

    if (strcmp(type, "error") == 0)
    {
        const char *msg = doc["error"]["message"] | "unknown realtime error";
        lock();
        g.fatalError = true;
        g.error = msg;
        unlock();
        Serial.printf("[realtime] error: %s\n", msg);
        return;
    }

    if (strcmp(type, "session.created") == 0)
    {
        lock();
        g.sessionCreated = true;
        unlock();
        Serial.println("[realtime] session.created");
        return;
    }

    if (strcmp(type, "session.updated") == 0)
    {
        lock();
        g.sessionUpdated = true;
        unlock();
        Serial.println("[realtime] session.updated");
        return;
    }

    // Audio playback: decode and enqueue to audio_out as soon as each chunk
    // arrives, instead of waiting for the full response.
    if (strcmp(type, "response.output_audio.delta") == 0)
    {
        const char *delta = doc["delta"] | "";
        const size_t deltaLen = strlen(delta);
        if (deltaLen > 0)
        {
            // static: avoids an allocation on the common hot path.
            static uint8_t decodeBuf[kAudioDeltaDecodeBufBytes];
            size_t decodedLen = 0;
            const int rc = mbedtls_base64_decode(
                decodeBuf, sizeof(decodeBuf), &decodedLen,
                reinterpret_cast<const unsigned char *>(delta), deltaLen);
            if (rc == 0)
            {
                audio_out::enqueuePcm16Mono(
                    reinterpret_cast<const int16_t *>(decodeBuf), decodedLen / sizeof(int16_t));
            }
            else if (rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && decodedLen > 0)
            {
                // Rare: this chunk's decoded size exceeds the static scratch
                // buffer. On BUFFER_TOO_SMALL, mbedtls has already written
                // the exact required size into decodedLen, so allocate a
                // one-off PSRAM buffer of that size instead of dropping the
                // chunk (dropping chunks previously caused near-total
                // silence despite the session/API side working correctly).
                uint8_t *big =
                    static_cast<uint8_t *>(heap_caps_malloc(decodedLen, MALLOC_CAP_SPIRAM));
                if (big != nullptr)
                {
                    size_t bigLen = 0;
                    if (mbedtls_base64_decode(
                            big, decodedLen, &bigLen,
                            reinterpret_cast<const unsigned char *>(delta), deltaLen) == 0)
                    {
                        audio_out::enqueuePcm16Mono(
                            reinterpret_cast<const int16_t *>(big), bigLen / sizeof(int16_t));
                    }
                    heap_caps_free(big);
                    Serial.printf(
                        "[realtime] output_audio.delta > %u байт, обработан через PSRAM (%u байт)\n",
                        static_cast<unsigned>(sizeof(decodeBuf)), static_cast<unsigned>(decodedLen));
                }
                else
                {
                    Serial.println(
                        "[realtime] output_audio.delta: не удалось выделить PSRAM, пропущен");
                }
            }
            else
            {
                Serial.printf("[realtime] output_audio.delta decode error (%d), пропущен\n", rc);
            }
        }
        return;
    }

    // Transcript matching the audio above; accumulated the same way text
    // deltas were before audio output was added.
    if (strcmp(type, "response.output_audio_transcript.delta") == 0 ||
        strcmp(type, "response.output_text.delta") == 0 ||
        // Some deployments still emit the older transcript event name for text.
        strcmp(type, "response.text.delta") == 0)
    {
        const char *delta = doc["delta"] | "";
        if (delta[0] != '\0')
        {
            lock();
            g.outputText += delta;
            unlock();
        }
        return;
    }

    // Everything below needs fields the filter above dropped (call_id, name,
    // arguments, response.output, ...). These events are rare -- at most a
    // couple per turn -- so a second, unfiltered parse here is cheap
    // relative to the streaming deltas handled above.
    JsonDocument full;
    const DeserializationError fullErr = deserializeJson(full, json);
    if (fullErr)
    {
        Serial.printf("[realtime] skip unparseable event (%s)\n", fullErr.c_str());
        return;
    }

    if (strcmp(type, "response.function_call_arguments.done") == 0)
    {
        const char *callId = full["call_id"] | "";
        const char *name = full["name"] | "";
        const char *args = full["arguments"] | "";
        lock();
        g.callId = callId;
        g.callName = name;
        g.callArgs = args;
        g.haveFunctionCall = (g.callId.length() > 0);
        unlock();
        Serial.printf("[realtime] function_call args done name=%s\n", name);
        return;
    }

    if (strcmp(type, "response.done") == 0)
    {
        // Prefer complete items from response.output when deltas were empty.
        JsonArrayConst output = full["response"]["output"].as<JsonArrayConst>();
        for (JsonVariantConst item : output)
        {
            const char *itemType = item["type"] | "";
            if (strcmp(itemType, "function_call") == 0)
            {
                const char *callId = item["call_id"] | "";
                const char *name = item["name"] | "";
                const char *args = item["arguments"] | "";
                lock();
                if (!g.haveFunctionCall && callId[0] != '\0')
                {
                    g.callId = callId;
                    g.callName = name;
                    g.callArgs = args;
                    g.haveFunctionCall = true;
                }
                unlock();
            }
            else if (strcmp(itemType, "message") == 0)
            {
                for (JsonVariantConst part : item["content"].as<JsonArrayConst>())
                {
                    const char *partType = part["type"] | "";
                    // "output_audio" parts carry the transcript (not the
                    // audio itself -- that only ever arrives via the delta
                    // events above) under "transcript"; plain text parts
                    // use "text". Fall back to whichever is non-empty.
                    const char *partText = part["text"] | "";
                    if (partText[0] == '\0')
                    {
                        partText = part["transcript"] | "";
                    }
                    if ((strcmp(partType, "output_audio") == 0 ||
                         strcmp(partType, "output_text") == 0 ||
                         strcmp(partType, "text") == 0) &&
                        partText[0] != '\0')
                    {
                        lock();
                        if (g.outputText.length() == 0)
                        {
                            g.outputText = partText;
                        }
                        unlock();
                    }
                }
            }
        }

        lock();
        g.responseDone = true;
        unlock();
        Serial.println("[realtime] response.done");
        return;
    }
}

void onWsEvent(void * /*handler_args*/, esp_event_base_t /*base*/, int32_t event_id, void *event_data)
{
    const esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t *>(event_data);

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        lock();
        g.connected = true;
        unlock();
        Serial.println("[realtime] WebSocket connected");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        lock();
        g.connected = false;
        unlock();
        Serial.println("[realtime] WebSocket disconnected");
        break;

    case WEBSOCKET_EVENT_ERROR:
        lock();
        g.fatalError = true;
        if (g.error.length() == 0)
        {
            g.error = "WebSocket transport error";
        }
        unlock();
        Serial.println("[realtime] WebSocket error");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data == nullptr || data->op_code != WEBSOCKET_OPCODE_TEXT)
        {
            break;
        }
        // Reassemble fragmented frames.
        if (data->payload_offset == 0)
        {
            lock();
            g.rxBuf = "";
            g.rxPayloadLen = data->payload_len;
            g.rxGot = 0;
            g.rxBuf.reserve(static_cast<unsigned>(data->payload_len > 0 ? data->payload_len : 256));
            unlock();
        }
        if (data->data_ptr != nullptr && data->data_len > 0)
        {
            lock();
            g.rxBuf.concat(
                reinterpret_cast<const char *>(data->data_ptr),
                static_cast<unsigned int>(data->data_len));
            g.rxGot += data->data_len;
            const bool complete = (g.rxPayloadLen > 0 && g.rxGot >= g.rxPayloadLen) ||
                                  (data->payload_offset + data->data_len >= data->payload_len);
            String completeMsg;
            if (complete)
            {
                completeMsg = g.rxBuf;
                g.rxBuf = "";
                g.rxGot = 0;
                g.rxPayloadLen = 0;
            }
            unlock();
            if (completeMsg.length() > 0)
            {
                handleServerEvent(completeMsg);
            }
        }
        break;

    default:
        break;
    }
}

bool waitFlag(bool (*pred)(), uint32_t timeoutMs)
{
    const uint32_t start = millis();
    while (millis() - start < timeoutMs)
    {
        if (g.fatalError)
        {
            return false;
        }
        if (pred())
        {
            return true;
        }
        delay(10);
    }
    return false;
}

/**
 * Linear-resample a [dstStart, dstStart + n) range (in destination-rate
 * sample indices) from the full source buffer directly into dstOut. Because
 * the whole source utterance is already resident in memory, each
 * destination sample can be computed independently from its global
 * position -- so callers can resample and upload in small chunks without
 * ever materializing a second full-length copy of the utterance.
 */
void resampleRange(
    const int16_t *src,
    size_t srcCount,
    uint32_t srcRate,
    uint32_t dstRate,
    size_t dstStart,
    size_t n,
    int16_t *dstOut)
{
    if (srcRate == dstRate)
    {
        size_t count = (dstStart < srcCount) ? std::min(n, srcCount - dstStart) : 0;
        if (count > 0)
        {
            memcpy(dstOut, src + dstStart, count * sizeof(int16_t));
        }
        if (count < n)
        {
            memset(dstOut + count, 0, (n - count) * sizeof(int16_t));
        }
        return;
    }

    for (size_t j = 0; j < n; j++)
    {
        const size_t i = dstStart + j;
        const float srcPos =
            static_cast<float>(i) * static_cast<float>(srcRate) / static_cast<float>(dstRate);
        size_t i0 = static_cast<size_t>(srcPos);
        if (i0 >= srcCount)
        {
            i0 = srcCount - 1;
        }
        size_t i1 = i0 + 1;
        if (i1 >= srcCount)
        {
            i1 = srcCount - 1;
        }
        const float t = srcPos - static_cast<float>(i0);
        const float s0 = static_cast<float>(src[i0]);
        const float s1 = static_cast<float>(src[i1]);
        dstOut[j] = static_cast<int16_t>(s0 + (s1 - s0) * t);
    }
}

bool sendPcmBase64(const int16_t *pcm, size_t sampleCount)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(pcm);
    const size_t byteLen = sampleCount * sizeof(int16_t);

    // static: avoids re-allocating ~4 KB on the stack for every chunk sent.
    static uint8_t outBuf[base64EncodedLen(kEncodeChunkBytes) + 4];

    String msg;
    msg.reserve(base64EncodedLen(kEncodeChunkBytes) + 64);

    size_t offset = 0;
    while (offset < byteLen)
    {
        size_t chunk = byteLen - offset;
        if (chunk > kEncodeChunkBytes)
        {
            chunk = kEncodeChunkBytes;
        }

        size_t outLen = 0;
        if (mbedtls_base64_encode(outBuf, sizeof(outBuf), &outLen, bytes + offset, chunk) != 0)
        {
            return false;
        }

        // Single contiguous build instead of a byte-by-byte += loop: one
        // allocation (already reserved above) instead of O(outLen) of them.
        msg = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
        msg.concat(reinterpret_cast<const char *>(outBuf), outLen);
        msg += "\"}";

        if (!sendText(msg))
        {
            // Possible transient backpressure on the websocket TX queue;
            // give its task a moment to drain and retry once before failing.
            delay(5);
            if (!sendText(msg))
            {
                return false;
            }
        }
        offset += chunk;
        taskYIELD();
    }
    return true;
}

bool submitFunctionOutput(const String &callId, const String &outputJson)
{
    String item = "{";
    item += "\"type\":\"conversation.item.create\",";
    item += "\"item\":{";
    item += "\"type\":\"function_call_output\",";
    item += "\"call_id\":\"";
    item += openai_http::jsonEscape(callId.c_str());
    item += "\",";
    item += "\"output\":\"";
    item += openai_http::jsonEscape(outputJson.c_str());
    item += "\"";
    item += "}";
    item += "}";

    if (!sendText(item))
    {
        return false;
    }

    // Continue the original turn; spoken (audio) output with transcript.
    return sendText("{\"type\":\"response.create\",\"response\":{\"output_modalities\":[\"audio\"]}}");
}

String extractQuestion(const String &argsJson)
{
    JsonDocument doc;
    if (deserializeJson(doc, argsJson))
    {
        return String();
    }
    const char *q = doc["question"] | "";
    return String(q);
}

/** Clears per-turn state. Deliberately leaves connected/sessionCreated/
 *  sessionUpdated untouched -- those reflect the live WebSocket session,
 *  which may be reused across multiple calls. */
void resetTurnState()
{
    lock();
    g.fatalError = false;
    g.error = "";
    g.rxBuf = "";
    g.rxPayloadLen = 0;
    g.rxGot = 0;
    g.outputText = "";
    g.callId = "";
    g.callName = "";
    g.callArgs = "";
    g.haveFunctionCall = false;
    g.responseDone = false;
    g.functionRound = false;
    unlock();
}

/** Closes and destroys the current client, if any, and clears connection
 *  state. Safe to call when there is no client (no-op). */
void teardownClient()
{
    if (g.client != nullptr)
    {
        esp_websocket_client_close(g.client, pdMS_TO_TICKS(2000));
        esp_websocket_client_destroy(g.client);
        g.client = nullptr;
    }
    lock();
    g.connected = false;
    g.sessionCreated = false;
    g.sessionUpdated = false;
    unlock();
}

/** True when the existing client is connected and has a configured session,
 *  i.e. it can be reused for a new utterance without reconnecting. */
bool isSessionUsable()
{
    if (g.client == nullptr)
    {
        return false;
    }
    lock();
    const bool updated = g.sessionUpdated;
    const bool fatal = g.fatalError;
    unlock();
    return updated && !fatal && esp_websocket_client_is_connected(g.client);
}

/** Opens a new WebSocket, waits for session.created, sends session.update,
 *  and waits for session.updated. On any failure, tears the client back
 *  down and reports the error via errorOut. */
bool connectSession(const Config &config, uint32_t sampleRateHz, String &errorOut)
{
    // Websocket events are delivered on the default ESP event loop.
    static bool sEventLoopReady = false;
    if (!sEventLoopReady)
    {
        const esp_err_t loopErr = esp_event_loop_create_default();
        if (loopErr != ESP_OK && loopErr != ESP_ERR_INVALID_STATE)
        {
            errorOut = "esp_event_loop_create_default failed";
            return false;
        }
        sEventLoopReady = true;
    }

    String headers = "Authorization: Bearer ";
    headers += config.apiKey;
    headers += "\r\n";

    String uri = "wss://api.openai.com/v1/realtime?model=";
    uri += config.realtimeModel;

    esp_websocket_client_config_t wsCfg = {};
    wsCfg.uri = uri.c_str();
    wsCfg.headers = headers.c_str();
    wsCfg.cert_pem = openai_tls::rootCertsPem();
    wsCfg.disable_auto_reconnect = true;
    wsCfg.buffer_size = 4096;
    wsCfg.task_stack = 8192;
    wsCfg.ping_interval_sec = 20;
    wsCfg.skip_cert_common_name_check = false;

    g.client = esp_websocket_client_init(&wsCfg);
    if (g.client == nullptr)
    {
        errorOut = "esp_websocket_client_init failed";
        return false;
    }

    esp_websocket_register_events(g.client, WEBSOCKET_EVENT_ANY, onWsEvent, nullptr);
    if (esp_websocket_client_start(g.client) != ESP_OK)
    {
        errorOut = "WebSocket start failed";
        teardownClient();
        return false;
    }

    auto connectedPred = []() {
        lock();
        const bool v = g.connected;
        unlock();
        return v;
    };
    if (!waitFlag(connectedPred, kConnectTimeoutMs))
    {
        errorOut = g.error.length() ? g.error : "Таймаут подключения Realtime WebSocket";
        teardownClient();
        return false;
    }

    auto createdPred = []() {
        lock();
        const bool v = g.sessionCreated;
        unlock();
        return v;
    };
    if (!waitFlag(createdPred, kConnectTimeoutMs))
    {
        errorOut = g.error.length() ? g.error : "Нет session.created";
        teardownClient();
        return false;
    }

    const String update = buildSessionUpdate(config, sampleRateHz);
    if (!sendText(update))
    {
        errorOut = "Не удалось отправить session.update";
        teardownClient();
        return false;
    }

    auto updatedPred = []() {
        lock();
        const bool v = g.sessionUpdated;
        unlock();
        return v;
    };
    if (!waitFlag(updatedPred, kConnectTimeoutMs))
    {
        errorOut = g.error.length() ? g.error : "Нет session.updated";
        teardownClient();
        return false;
    }

    return true;
}

/** Reuses the current session if it is still usable; otherwise reconnects
 *  from scratch. On failure, result.error is set. */
bool ensureSession(const Config &config, uint32_t sampleRateHz, Result &result)
{
    if (isSessionUsable())
    {
        return true;
    }
    teardownClient(); // no-op if already torn down; clears any half-open state
    String err;
    if (!connectSession(config, sampleRateHz, err))
    {
        result.error = err;
        return false;
    }
    return true;
}

} // namespace

Result askUtterance(
    const Config &config,
    const int16_t *pcmSamples,
    size_t sampleCount,
    uint32_t srcSampleRateHz)
{
    Result result;

    if (WiFi.status() != WL_CONNECTED)
    {
        result.error = "Wi-Fi не подключён";
        return result;
    }
    if (pcmSamples == nullptr || sampleCount == 0)
    {
        result.error = "Пустая запись";
        return result;
    }

    if (g.mu == nullptr)
    {
        g.mu = xSemaphoreCreateMutex();
    }
    resetTurnState();

    if (!ensureSession(config, kRealtimeSampleRateHz, result))
    {
        return result;
    }

    const size_t dstCount = (srcSampleRateHz == kRealtimeSampleRateHz)
        ? sampleCount
        : static_cast<size_t>(
              (static_cast<uint64_t>(sampleCount) * kRealtimeSampleRateHz) / srcSampleRateHz);

    Serial.printf(
        "[realtime] uploading PCM: %u samples @ %u Hz -> %u samples @ %u Hz\n",
        static_cast<unsigned>(sampleCount), static_cast<unsigned>(srcSampleRateHz),
        static_cast<unsigned>(dstCount), static_cast<unsigned>(kRealtimeSampleRateHz));

    // Resample+send in small chunks so neither a full-length resampled copy
    // nor an oversized single WS frame is ever needed; local VAD already
    // delimited the utterance, so server turn_detection stays null.
    static int16_t chunkBuf[kPcmChunkSamples];
    size_t sentDst = 0;
    while (sentDst < dstCount)
    {
        size_t n = dstCount - sentDst;
        if (n > kPcmChunkSamples)
        {
            n = kPcmChunkSamples;
        }
        resampleRange(pcmSamples, sampleCount, srcSampleRateHz, kRealtimeSampleRateHz, sentDst, n, chunkBuf);
        if (!sendPcmBase64(chunkBuf, n))
        {
            result.error = "Ошибка отправки audio append";
            teardownClient();
            return result;
        }
        sentDst += n;
    }

    if (!sendText("{\"type\":\"input_audio_buffer.commit\"}") ||
        !sendText("{\"type\":\"response.create\",\"response\":{\"output_modalities\":[\"audio\"]}}"))
    {
        result.error = "Ошибка commit/response.create";
        teardownClient();
        return result;
    }

    Serial.println("[realtime] жду ответ модели…");

    bool webSearchUsed = false;
    String citations;
    bool toolExecuted = false; // prevent recursive tool loops
    const uint32_t deadline = millis() + kSessionTimeoutMs;

    while (millis() < deadline)
    {
        if (g.fatalError)
        {
            result.error = g.error;
            break;
        }

        bool responseDone = false;
        bool haveFn = false;
        String callId;
        String callName;
        String callArgs;
        String textSnap;

        lock();
        responseDone = g.responseDone;
        haveFn = g.haveFunctionCall;
        if (haveFn)
        {
            callId = g.callId;
            callName = g.callName;
            callArgs = g.callArgs;
        }
        textSnap = g.outputText;
        unlock();

        if (!responseDone)
        {
            delay(20);
            continue;
        }

        if (haveFn && callName == kToolName && !toolExecuted)
        {
            toolExecuted = true;
            const String question = extractQuestion(callArgs);
            Serial.printf(
                "[realtime] модель запросила web-assisted answer (q_len=%u)\n",
                static_cast<unsigned>(question.length()));

            const openai_responses::Config respCfg{
                config.apiKey,
                config.responsesModel,
                config.instructions,
                config.reasoningEffort,
            };
            const openai_responses::Result resp = openai_responses::askWithWebSearch(respCfg, question.c_str());

            String outputJson;
            if (resp.ok)
            {
                webSearchUsed = resp.usedWebSearch;
                citations = resp.citations;
                outputJson = "{";
                outputJson += "\"answer\":\"";
                outputJson += openai_http::jsonEscape(resp.text.c_str());
                outputJson += "\",";
                outputJson += "\"used_web_search\":";
                outputJson += resp.usedWebSearch ? "true" : "false";
                outputJson += ",";
                outputJson += "\"citations\":\"";
                outputJson += openai_http::jsonEscape(resp.citations.c_str());
                outputJson += "\"";
                outputJson += "}";
                Serial.printf(
                    "[realtime] Responses ok used_web_search=%d citations_len=%u\n",
                    resp.usedWebSearch ? 1 : 0, static_cast<unsigned>(resp.citations.length()));
            }
            else
            {
                outputJson = "{";
                outputJson += "\"error\":\"";
                outputJson += openai_http::jsonEscape(resp.error.c_str());
                outputJson += "\"";
                outputJson += "}";
                Serial.print("[realtime] Responses error: ");
                Serial.println(resp.error);
            }

            lock();
            g.responseDone = false;
            g.haveFunctionCall = false;
            g.callId = "";
            g.callName = "";
            g.callArgs = "";
            g.outputText = "";
            g.functionRound = true;
            unlock();

            if (!submitFunctionOutput(callId, outputJson))
            {
                result.error = "Не удалось отправить function_call_output";
                break;
            }
            continue;
        }

        // Final text response (no pending tool, or tool already executed).
        if (textSnap.length() > 0)
        {
            result.ok = true;
            result.text = textSnap;
            result.usedWebSearch = webSearchUsed;
            result.citations = citations;
            break;
        }

        if (haveFn && toolExecuted)
        {
            result.error = "Модель повторно вызвала инструмент — остановлено (anti-loop)";
            break;
        }

        result.error = "response.done без текста и без function_call";
        break;
    }

    if (!result.ok && result.error.length() == 0)
    {
        result.error = "Таймаут ожидания ответа Realtime";
    }

    if (result.ok)
    {
        // response.done means the model finished generating, not that
        // audio_out has finished playing it -- the ring buffer may still
        // hold the tail of the answer. Wait for it to drain to the DAC
        // before returning, so the caller doesn't resume the mic (and risk
        // picking up the speaker) or start a new turn mid-sentence.
        if (!audio_out::waitDrained(kDrainTimeoutMs))
        {
            Serial.println("[realtime] audio_out не успел доиграть ответ за таймаут");
        }

        // Keep the socket + session alive for the next utterance instead of
        // tearing it down -- just clear any residual server-side audio
        // buffer so the next turn starts clean. If this send fails, the
        // next askUtterance() call will detect a dead session and
        // reconnect from scratch.
        sendText("{\"type\":\"input_audio_buffer.clear\"}");
    }
    else
    {
        // Don't leave stale/partial audio queued if the turn failed.
        audio_out::reset();
        teardownClient();
    }

    return result;
}

} // namespace openai_realtime
