#include "audio_out.h"

#include <Arduino.h>
#include <I2S.h> // pulls in the raw ESP-IDF i2s driver under the esp_i2s:: namespace
#include <atomic>
#include <esp_heap_caps.h>

namespace audio_out
{

namespace
{

/** ~20 s of stereo PCM16 @ 24 kHz (~1.83 MB in PSRAM). The Realtime API
 *  streams audio.delta chunks as fast as the model generates/the network
 *  delivers them, which in practice is noticeably faster than real-time
 *  playback (the consumer here is deliberately paced to real time, see
 *  playbackTask). A ~1 s ring therefore filled up and started dropping
 *  audio within the first second of almost any spoken reply. PSRAM is
 *  abundant (several MB free), so size this to comfortably cover a full
 *  utterance instead of only smoothing scheduling jitter; dropping is now
 *  only a last-resort safety net for pathologically long responses. */
constexpr size_t kRingCapacityFrames = kSampleRateHz * 20;
/** Frames (stereo samples) handed to i2s_write() per call: 20 ms @ 24 kHz. */
constexpr size_t kWriteChunkFrames = 480;

int16_t *gRing = nullptr; // interleaved L,R, PSRAM; kRingCapacityFrames frames
size_t gHead = 0;         // next frame index to write (producer)
size_t gTail = 0;         // next frame index to read (consumer)
size_t gCount = 0;        // frames currently buffered
SemaphoreHandle_t gMutex = nullptr;
TaskHandle_t gTaskHandle = nullptr;
std::atomic<bool> gPlaying{false};
float gVolume = 1.0f;

void playbackTask(void * /*arg*/)
{
    static int16_t chunk[kWriteChunkFrames * 2]; // interleaved stereo scratch

    for (;;)
    {
        xSemaphoreTake(gMutex, portMAX_DELAY);
        const size_t avail = gCount;
        xSemaphoreGive(gMutex);

        if (avail == 0)
        {
            gPlaying.store(false, std::memory_order_relaxed);
            delay(2);
            continue;
        }

        gPlaying.store(true, std::memory_order_relaxed);
        const size_t n = (avail < kWriteChunkFrames) ? avail : kWriteChunkFrames;

        xSemaphoreTake(gMutex, portMAX_DELAY);
        for (size_t i = 0; i < n; i++)
        {
            chunk[i * 2] = gRing[gTail * 2];
            chunk[i * 2 + 1] = gRing[gTail * 2 + 1];
            gTail = (gTail + 1) % kRingCapacityFrames;
        }
        gCount -= n;
        xSemaphoreGive(gMutex);

        // Blocking is intentional and harmless here: this task's only job
        // is to feed the DAC, so waiting for DMA buffer space is exactly
        // the backpressure we want (mirrors audio_capture's i2s_read).
        size_t bytesWritten = 0;
        const size_t bytesToWrite = n * 2 * sizeof(int16_t);
        const esp_err_t writeErr = esp_i2s::i2s_write(
            esp_i2s::I2S_NUM_1, chunk, bytesToWrite, &bytesWritten, portMAX_DELAY);
        if (writeErr != ESP_OK || bytesWritten != bytesToWrite)
        {
            Serial.printf(
                "[audio_out] i2s_write failed (err=%d, wrote %u/%u bytes)\n",
                static_cast<int>(writeErr), static_cast<unsigned>(bytesWritten),
                static_cast<unsigned>(bytesToWrite));
        }
    }
}

} // namespace

bool begin(int pinBck, int pinLrck, int pinDin, float volume)
{
    gVolume = volume;

    gRing = static_cast<int16_t *>(
        heap_caps_malloc(kRingCapacityFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (gRing == nullptr)
    {
        return false;
    }

    gMutex = xSemaphoreCreateMutex();
    if (gMutex == nullptr)
    {
        return false;
    }

    esp_i2s::i2s_config_t i2sConfig = {};
    i2sConfig.mode = static_cast<esp_i2s::i2s_mode_t>(
        esp_i2s::I2S_MODE_MASTER | esp_i2s::I2S_MODE_TX);
    i2sConfig.sample_rate = kSampleRateHz;
    i2sConfig.bits_per_sample = esp_i2s::I2S_BITS_PER_SAMPLE_16BIT;
    i2sConfig.channel_format = esp_i2s::I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2sConfig.communication_format = esp_i2s::I2S_COMM_FORMAT_STAND_I2S;
    i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2sConfig.dma_buf_count = 8;
    i2sConfig.dma_buf_len = 240; // frames/buffer, ~10 ms at 24 kHz (~80 ms total DMA depth)
    // APLL instead of the APB-derived divider: at 24 kHz the APB divider
    // can't hit an exact bit-clock ratio, so BCK ends up with enough jitter
    // for PCM5102A to audibly crackle/hiss even though the data itself is
    // correct. APLL gives a much more precise clock for this sample rate.
    i2sConfig.use_apll = true;
    i2sConfig.tx_desc_auto_clear = true; // output silence (not noise) on underrun

    if (esp_i2s::i2s_driver_install(esp_i2s::I2S_NUM_1, &i2sConfig, 0, nullptr) != ESP_OK)
    {
        return false;
    }

    esp_i2s::i2s_pin_config_t pinConfig = {};
    pinConfig.mck_io_num = I2S_PIN_NO_CHANGE; // PCM5102A derives its clock internally
    pinConfig.bck_io_num = pinBck;
    pinConfig.ws_io_num = pinLrck;
    pinConfig.data_out_num = pinDin;
    pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

    if (esp_i2s::i2s_set_pin(esp_i2s::I2S_NUM_1, &pinConfig) != ESP_OK)
    {
        esp_i2s::i2s_driver_uninstall(esp_i2s::I2S_NUM_1);
        return false;
    }

    const BaseType_t ok = xTaskCreate(playbackTask, "AudioOut", 1024 * 4, nullptr, 10, &gTaskHandle);
    if (ok != pdPASS)
    {
        return false;
    }

    Serial.printf(
        "[audio_out] I2S TX готов: BCK=%d LRCK=%d DIN=%d @ %u Hz\n",
        pinBck, pinLrck, pinDin, static_cast<unsigned>(kSampleRateHz));
    return true;
}

void enqueuePcm16Mono(const int16_t *samples, size_t count)
{
    if (gRing == nullptr || samples == nullptr || count == 0)
    {
        return;
    }

    // One-shot confirmation that audio is actually reaching this module at
    // all -- distinguishes "no response.output_audio.delta ever arrived"
    // (Realtime/session problem) from "it arrived but there's no sound"
    // (I2S/wiring problem) when debugging on Serial.
    static bool everEnqueued = false;
    if (!everEnqueued)
    {
        everEnqueued = true;
        Serial.printf(
            "[audio_out] первый аудио-чанк получен (%u samples)\n", static_cast<unsigned>(count));
    }

    xSemaphoreTake(gMutex, portMAX_DELAY);
    const size_t space = kRingCapacityFrames - gCount;
    const size_t toWrite = (count < space) ? count : space;
    for (size_t i = 0; i < toWrite; i++)
    {
        int32_t scaled = static_cast<int32_t>(samples[i] * gVolume);
        if (scaled > 32767)
        {
            scaled = 32767;
        }
        else if (scaled < -32768)
        {
            scaled = -32768;
        }
        const int16_t s = static_cast<int16_t>(scaled);
        gRing[gHead * 2] = s;     // left
        gRing[gHead * 2 + 1] = s; // right
        gHead = (gHead + 1) % kRingCapacityFrames;
    }
    gCount += toWrite;
    xSemaphoreGive(gMutex);

    if (toWrite < count)
    {
        // Ring is full: drop the excess instead of blocking the WebSocket
        // event callback (which would otherwise stall processing of other
        // events, e.g. response.done/errors, behind a full playback buffer).
        Serial.printf(
            "[audio_out] ring full, dropped %u/%u samples\n",
            static_cast<unsigned>(count - toWrite), static_cast<unsigned>(count));
    }
}

bool waitDrained(uint32_t timeoutMs)
{
    const uint32_t start = millis();
    while (millis() - start < timeoutMs)
    {
        xSemaphoreTake(gMutex, portMAX_DELAY);
        const size_t avail = gCount;
        xSemaphoreGive(gMutex);
        if (avail == 0 && !gPlaying.load(std::memory_order_relaxed))
        {
            return true;
        }
        delay(5);
    }
    return false;
}

void reset()
{
    xSemaphoreTake(gMutex, portMAX_DELAY);
    gHead = 0;
    gTail = 0;
    gCount = 0;
    xSemaphoreGive(gMutex);
    esp_i2s::i2s_zero_dma_buffer(esp_i2s::I2S_NUM_1);
}

bool isPlaying()
{
    xSemaphoreTake(gMutex, portMAX_DELAY);
    const size_t avail = gCount;
    xSemaphoreGive(gMutex);
    return avail > 0 || gPlaying.load(std::memory_order_relaxed);
}

} // namespace audio_out
