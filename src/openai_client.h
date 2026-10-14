#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

/**
 * High-level OpenAI entry point used by main.cpp.
 *
 * Approach B: Realtime WebSocket (audio in, audio out + transcript) with
 * ESP32-side orchestration of Responses + built-in web_search when the
 * model requests it. Audio output is streamed live to audio_out as it
 * arrives, through a PCM5102A (or similar) I2S DAC.
 */
namespace openai
{

struct Config
{
    const char *apiKey;
    const char *realtimeModel;
    const char *responsesModel;
    const char *systemPrompt;
    const char *voice; // Realtime output voice, e.g. "marin"
    const char *reasoningEffort; // Responses reasoning.effort, e.g. "low"
};

struct Result
{
    bool ok = false;
    bool usedWebSearch = false;
    String text; // transcript of the spoken answer
    String citations;
    String error;
};

/**
 * Send one complete WAV clip (header + PCM16 mono). The WAV header is
 * stripped before the Realtime upload; sample rate is taken from the audio
 * capture config. Blocks until the spoken answer has finished playing
 * through audio_out.
 */
Result askAudio(const Config &config, const uint8_t *wavBytes, size_t wavLen);

} // namespace openai
