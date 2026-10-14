#include "openai_responses.h"

#include "openai_http.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

namespace openai_responses
{

namespace
{

constexpr uint32_t kTimeoutSec = 90;

/**
 * Allocates ArduinoJson's internal document storage in PSRAM instead of the
 * much smaller internal heap. Responses payloads that include web_search
 * results/citations can run into several KB of JSON, which is exactly the
 * case this is meant for (see the official ArduinoJson ESP32 example).
 */
class SpiRamAllocator : public ArduinoJson::Allocator
{
public:
    void *allocate(size_t size) override
    {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    void deallocate(void *pointer) override
    {
        heap_caps_free(pointer);
    }

    void *reallocate(void *ptr, size_t newSize) override
    {
        return heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM);
    }
};

} // namespace

Result askWithWebSearch(const Config &config, const char *question)
{
    Result result;

    if (WiFi.status() != WL_CONNECTED)
    {
        result.error = "Wi-Fi не подключён";
        return result;
    }
    if (question == nullptr || question[0] == '\0')
    {
        result.error = "Пустой вопрос для Responses";
        return result;
    }

    // Built-in web_search with automatic tool selection — the model searches
    // only when the question needs current information.
    String body = "{";
    body += "\"model\":\"";
    body += config.model;
    body += "\",";
    body += "\"instructions\":\"";
    body += openai_http::jsonEscape(config.instructions);
    body += "\",";
    body += "\"tools\":[{\"type\":\"web_search\"}],";
    body += "\"tool_choice\":\"auto\",";
    body += "\"include\":[\"web_search_call.action.sources\"],";
    // Keep reasoning cheap/fast for voice turnaround. GPT-6 family often
    // rejects effort "none"; "low" is the practical minimum.
    if (config.reasoningEffort != nullptr && config.reasoningEffort[0] != '\0')
    {
        body += "\"reasoning\":{\"effort\":\"";
        body += config.reasoningEffort;
        body += "\"},";
    }
    body += "\"input\":\"";
    body += openai_http::jsonEscape(question);
    body += "\"";
    body += "}";

    Serial.printf(
        "[responses] POST /v1/responses model=%s question_len=%u body_len=%u\n",
        config.model, static_cast<unsigned>(strlen(question)),
        static_cast<unsigned>(body.length()));

    openai_http::HttpResponse http;
    if (!openai_http::postJson(config.apiKey, "/v1/responses", body, kTimeoutSec, http))
    {
        result.error = "Не удалось выполнить Responses HTTP запрос";
        return result;
    }

    Serial.printf(
        "[responses] HTTP %d body=%u байт\n", http.status,
        static_cast<unsigned>(http.body.length()));

    if (http.status != 200)
    {
        result.error = "HTTP " + String(http.status) + ": " + http.body.substring(0, 400);
        return result;
    }

    // Responses payloads with search results can be large; allocate in PSRAM.
    SpiRamAllocator spiRamAllocator;
    JsonDocument doc(&spiRamAllocator);
    const DeserializationError parseErr = deserializeJson(doc, http.body);
    if (parseErr)
    {
        result.error = String("JSON parse: ") + parseErr.c_str();
        return result;
    }

    String text;
    String citations;
    bool usedSearch = false;

    JsonArrayConst output = doc["output"].as<JsonArrayConst>();
    for (JsonVariantConst item : output)
    {
        const char *type = item["type"] | "";
        if (strcmp(type, "web_search_call") == 0)
        {
            usedSearch = true;
            JsonArrayConst sources = item["action"]["sources"].as<JsonArrayConst>();
            for (JsonVariantConst src : sources)
            {
                const char *url = src["url"] | static_cast<const char *>(nullptr);
                if (url != nullptr)
                {
                    if (citations.length() > 0)
                    {
                        citations += "\n";
                    }
                    citations += url;
                }
            }
            continue;
        }

        if (strcmp(type, "message") != 0)
        {
            continue;
        }

        for (JsonVariantConst part : item["content"].as<JsonArrayConst>())
        {
            const char *partType = part["type"] | "";
            if (strcmp(partType, "output_text") != 0)
            {
                continue;
            }

            const char *partText = part["text"] | "";
            if (partText[0] != '\0')
            {
                if (text.length() > 0)
                {
                    text += "\n";
                }
                text += partText;
            }

            for (JsonVariantConst ann : part["annotations"].as<JsonArrayConst>())
            {
                const char *annType = ann["type"] | "";
                if (strcmp(annType, "url_citation") != 0)
                {
                    continue;
                }
                usedSearch = true;
                const char *title = ann["title"] | "";
                const char *url = ann["url"] | "";
                if (url[0] == '\0')
                {
                    continue;
                }
                if (citations.length() > 0)
                {
                    citations += "\n";
                }
                if (title[0] != '\0')
                {
                    citations += title;
                    citations += " — ";
                }
                citations += url;
            }
        }
    }

    // Convenience field present on many SDK responses.
    if (text.length() == 0)
    {
        const char *outputText = doc["output_text"] | static_cast<const char *>(nullptr);
        if (outputText != nullptr)
        {
            text = outputText;
        }
    }

    if (text.length() == 0)
    {
        result.error = "Responses: пустой текст ответа";
        return result;
    }

    result.ok = true;
    result.usedWebSearch = usedSearch;
    result.text = text;
    result.citations = citations;
    return result;
}

} // namespace openai_responses
