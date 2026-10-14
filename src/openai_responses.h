#pragma once

#include <Arduino.h>

/** OpenAI Responses API with built-in web_search (tool_choice: auto). */
namespace openai_responses
{

struct Config
{
    const char *apiKey;
    const char *model;
    const char *instructions;
    const char *reasoningEffort; // e.g. "low"; nullptr/"" omits the field
};

struct Result
{
    bool ok = false;
    bool usedWebSearch = false;
    String text;       // answer including inline citations when present
    String citations;  // "title: url" lines extracted from url_citation annotations
    String error;
};

/**
 * Ask the Responses model a text question. The model chooses whether to call
 * the hosted web_search tool (tool_choice is "auto").
 */
Result askWithWebSearch(const Config &config, const char *question);

} // namespace openai_responses
