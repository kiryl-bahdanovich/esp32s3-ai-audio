#include "audio_capture.h"

#include <Arduino.h>
#include <I2S.h>
#include <atomic>

namespace audio
{

namespace
{

constexpr UBaseType_t kQueueDepth = 8;
/** PDM mic reads are quiet; amplify like the original KWS firmware did. */
constexpr int16_t kGain = 8;
/**
 * One-pole DC-blocking high-pass filter coefficient. PDM demodulation
 * commonly leaves a DC offset / slow drift on the signal; left unfiltered,
 * that offset inflates RMS/peak equally whether or not anyone is speaking,
 * which makes "silence" and "speech" look almost equally loud to the VAD.
 * R close to 1 gives a cutoff of a few tens of Hz at 16 kHz, well below
 * speech content but enough to remove DC/hum.
 */
constexpr float kDcBlockR = 0.995f;

QueueHandle_t gFrameQueue = nullptr;
std::atomic<bool> gPaused{false};

void captureTask(void * /*arg*/)
{
    static int16_t raw[kFrameSamples];
    const size_t bytesToRead = kFrameSamples * sizeof(int16_t);

    float dcPrevIn = 0.0f;
    float dcPrevOut = 0.0f;

    for (;;)
    {
        size_t bytesRead = 0;
        esp_i2s::i2s_read(esp_i2s::I2S_NUM_0, raw, bytesToRead, &bytesRead, portMAX_DELAY);

        if (bytesRead < bytesToRead)
        {
            // Partial read (short frame) -- drop it rather than feed VAD
            // with a shorter-than-expected buffer.
            continue;
        }

        if (gPaused.load(std::memory_order_relaxed))
        {
            continue;
        }

        for (size_t i = 0; i < kFrameSamples; i++)
        {
            // DC block first, on the raw signal.
            const float x = static_cast<float>(raw[i]);
            const float y = x - dcPrevIn + kDcBlockR * dcPrevOut;
            dcPrevIn = x;
            dcPrevOut = y;

            // Then apply gain. Multiplying can exceed int16 range (e.g. a
            // PDM demodulation click/pop) -- clamp instead of letting it
            // wrap around to a huge value of the opposite sign, which would
            // look like a very loud transient and falsely trigger VAD even
            // in a quiet room.
            float amplified = y * kGain;
            if (amplified > INT16_MAX)
            {
                amplified = INT16_MAX;
            }
            else if (amplified < INT16_MIN)
            {
                amplified = INT16_MIN;
            }
            raw[i] = static_cast<int16_t>(amplified);
        }

        // Non-blocking send: if the consumer fell behind and the queue is
        // full, drop this frame rather than stalling capture.
        xQueueSend(gFrameQueue, raw, 0);
    }
}

} // namespace

bool begin(int pinClk, int pinData)
{
    I2S.setAllPins(-1, pinClk, pinData, -1, -1);
    if (!I2S.begin(PDM_MONO_MODE, kSampleRateHz, kSampleBits))
    {
        return false;
    }

    gFrameQueue = xQueueCreate(kQueueDepth, kFrameSamples * sizeof(int16_t));
    if (gFrameQueue == nullptr)
    {
        return false;
    }

    // Locals here are a fixed-size int16 frame buffer plus a handful of
    // floats/ints for the DC-block filter -- no large stack users -- so 8 KB
    // leaves comfortable headroom without wasting half of a 16 KB stack.
    TaskHandle_t taskHandle = nullptr;
    const BaseType_t ok = xTaskCreate(
        captureTask, "AudioCapture", 1024 * 8, nullptr, 10, &taskHandle);
    return ok == pdPASS;
}

bool readFrame(int16_t *outFrame, uint32_t timeoutMs)
{
    return xQueueReceive(gFrameQueue, outFrame, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void pause()
{
    gPaused.store(true, std::memory_order_relaxed);
    xQueueReset(gFrameQueue);
}

void resume()
{
    xQueueReset(gFrameQueue);
    gPaused.store(false, std::memory_order_relaxed);
}

bool isPaused()
{
    return gPaused.load(std::memory_order_relaxed);
}

} // namespace audio
