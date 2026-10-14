#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Streamed I2S playback for a PCM5102A (or similar) stereo DAC.
 *
 * Realtime audio response deltas arrive as small mono PCM16 chunks at
 * kSampleRateHz; begin() installs a second I2S controller (I2S_NUM_1,
 * separate from the PDM microphone's I2S_NUM_0) in TX mode and starts a
 * background FreeRTOS task that continuously drains a PSRAM ring buffer
 * into the DAC. enqueuePcm16Mono() duplicates each mono sample to stereo
 * L/R and copies it into the ring, so playback starts as soon as the first
 * chunk arrives instead of waiting for the full response to be generated.
 */
namespace audio_out
{

/** Must match the Realtime API's audio output sample rate (session.update
 *  audio.output.format.rate). */
constexpr uint32_t kSampleRateHz = 24000;

/** Installs the I2S TX driver on the given pins and starts the background
 *  playback task. volume scales every sample before it hits the DAC
 *  (0.0 = silence, 1.0 = full; values above 1.0 amplify and may clip).
 *  False on failure (bad pins, driver install, or OOM). */
bool begin(int pinBck, int pinLrck, int pinDin, float volume = 1.0f);

/**
 * Copies count mono PCM16 samples into the playback ring buffer (duplicated
 * to stereo L/R). This is meant to be called from the Realtime WebSocket
 * event callback as audio deltas arrive, so it is kept non-blocking: if the
 * ring is full, the excess samples are dropped (and logged) rather than
 * blocking the caller, which would otherwise stall WebSocket event
 * processing.
 */
void enqueuePcm16Mono(const int16_t *samples, size_t count);

/**
 * Blocks until all buffered audio has been handed to the I2S driver (ring
 * empty and the playback task idle), or timeoutMs elapses. Call this after
 * a turn completes so the caller doesn't return/reconnect while the DAC is
 * still mid-sentence.
 */
bool waitDrained(uint32_t timeoutMs);

/** Discards any buffered-but-not-yet-played audio and silences the DAC
 *  immediately. Meant for aborting playback after an error, not normal use. */
void reset();

/** True while there is buffered or in-flight audio still being played. */
bool isPlaying();

} // namespace audio_out
