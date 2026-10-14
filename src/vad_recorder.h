#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Voice-activity-detecting recorder.
 *
 * Call poll() repeatedly (e.g. once per loop() iteration) while in the
 * "listening" state. It pulls frames from audio::readFrame(), tracks
 * speech/silence using simple RMS/peak thresholds, and accumulates PCM into
 * a pre-allocated PSRAM buffer (already wrapped in a WAV header) until the
 * speaker pauses for end_silence_ms or the max duration is reached.
 */
namespace vad
{

enum class Event
{
    None,
    UtteranceReady,
};

struct Config
{
    float startRms = 450.0f;
    int startPeak = 1200;
    float continueRms = 300.0f;
    int continuePeak = 900;
    uint32_t endSilenceMs = 1200;
    uint32_t minUtteranceMs = 400;
    // Also bounds peak PSRAM use for the WAV buffer (16 kHz x 16-bit mono).
    uint32_t maxRecordingMs = 12000;
    /** Pre-roll kept in a ring buffer so the first phoneme is not lost. */
    uint32_t preRollMs = 320;
};

/** Allocates the WAV buffer (header + max PCM) in PSRAM. False on OOM. */
bool begin(const Config &config);

/**
 * Samples ambient noise for durationMs (room should be quiet) and raises the
 * start/continue thresholds above the observed noise floor with a margin, so
 * a fixed set of defaults doesn't have to be guessed for every microphone /
 * environment. Thresholds never go below the values passed to begin().
 * Call this once, right after begin(), before the main loop starts polling.
 */
void calibrate(uint32_t durationMs = 1000);

/** Non-blocking-ish; consumes at most one audio frame per call. */
Event poll();

bool isRecording();

/** Complete WAV bytes (header + PCM), valid once Event::UtteranceReady fires. */
const uint8_t *wavData();
size_t wavLen();

/** Clears internal state so the next poll() starts a fresh utterance. */
void reset();

} // namespace vad
