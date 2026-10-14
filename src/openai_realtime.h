#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

/**
 * Approach B: Realtime WebSocket voice session + ESP32-side orchestration of
 * the Responses API built-in web_search tool. Output is streamed audio
 * (PCM16 @ 24 kHz, played live through audio_out as it arrives) plus the
 * matching transcript text.
 */
namespace openai_realtime
{

struct Config
{
    const char *apiKey;
    const char *realtimeModel;   // e.g. gpt-realtime-mini (see secrets.h)
    const char *responsesModel;  // e.g. gpt-4.1
    const char *instructions;
    const char *voice; // Realtime output voice, e.g. "marin"
    const char *reasoningEffort; // Responses reasoning.effort, e.g. "low"
};

struct Result
{
    bool ok = false;
    bool usedWebSearch = false;
    String text;
    String citations;
    String error;
};

/**
 * Send one PCM16 mono utterance (raw samples, no WAV header, captured at
 * srcSampleRateHz) over a Realtime WebSocket session with server VAD
 * disabled. Resampling to the Realtime API's input rate happens internally,
 * one small chunk at a time, so no second full-length copy of the utterance
 * is allocated. The Realtime model may call the get_web_assisted_answer
 * function; the ESP32 then runs Responses+web_search and returns
 * function_call_output so the Realtime model can finish with an audio
 * answer. Audio deltas are pushed to audio_out as they stream in and
 * Result::text carries the matching transcript (from
 * response.output_audio_transcript.delta); the call does not return until
 * audio_out has finished draining the response to the DAC.
 *
 * The underlying WebSocket session is kept open and reused across calls
 * (reconnecting only after an error, disconnect, or timeout) so repeated
 * utterances skip the TLS/WebSocket handshake.
 */
Result askUtterance(
    const Config &config,
    const int16_t *pcmSamples,
    size_t sampleCount,
    uint32_t srcSampleRateHz);

} // namespace openai_realtime
