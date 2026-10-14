#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Continuous PDM microphone capture.
 *
 * A background FreeRTOS task keeps reading from the I2S/PDM peripheral at all
 * times (so the DMA never overruns), and hands fixed-size frames to the
 * consumer via a queue. While paused, frames are read and discarded instead
 * of being queued, which is how the "don't listen while talking to OpenAI /
 * printing the answer" behaviour is implemented.
 */
namespace audio
{

constexpr uint32_t kSampleRateHz = 16000;
constexpr uint16_t kSampleBits = 16;

/** ~32 ms per frame at 16 kHz. */
constexpr size_t kFrameSamples = 512;

/** Starts I2S PDM capture and the background reader task. */
bool begin(int pinClk, int pinData);

/**
 * Blocks up to timeoutMs waiting for the next captured frame of
 * kFrameSamples int16 samples. Returns false on timeout (no frame available
 * yet, or capture is paused) -- the caller should simply try again.
 */
bool readFrame(int16_t *outFrame, uint32_t timeoutMs);

/** Frames are captured but dropped instead of being delivered to readFrame(). */
void pause();

/** Resume delivering frames to readFrame(). */
void resume();

bool isPaused();

} // namespace audio
