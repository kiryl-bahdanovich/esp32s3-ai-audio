#include "vad_recorder.h"

#include "audio_capture.h"
#include "wav_util.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>

namespace vad
{

namespace
{

enum class State
{
    Idle,
    Recording,
};

/** Idle/progress status lines are useful for debugging but printed on a hot
 *  polling path; disable in normal use to save Serial/CPU time and avoid
 *  heap churn from the formatted String temporaries. */
constexpr bool kVadVerbose = false;

constexpr uint8_t kStartDebounceFrames = 3;
constexpr uint32_t kFrameDurationMs =
    (audio::kFrameSamples * 1000) / audio::kSampleRateHz;
/** How often to print a progress line while recording, so it's obvious the
 *  firmware is still alive and listening instead of looking "stuck". */
constexpr uint32_t kStatusIntervalMs = 400;
/** Same, but while idle/waiting for speech to start. */
constexpr uint32_t kIdleStatusIntervalMs = 1000;
/** PDM mics commonly produce a startup click/pop in the first moments after
 *  I2S begins; discard this much audio before measuring the noise floor so
 *  that transient doesn't permanently inflate the VAD thresholds. */
constexpr uint32_t kCalibrationWarmupMs = 300;

Config gConfig;
State gState = State::Idle;

uint8_t *gWavBuffer = nullptr;
size_t gMaxSamples = 0;
size_t gRecordedSamples = 0;

uint32_t gSilenceMs = 0;
uint32_t gRecordedMs = 0;
uint32_t gLastStatusMs = 0;
uint32_t gLastIdleStatusMs = 0;
uint8_t gStartDebounce = 0;

/** Ring buffer of recent idle frames for pre-roll (allocated in begin()). */
int16_t *gPreRoll = nullptr;
size_t gPreRollFrames = 0;
size_t gPreRollWrite = 0;
size_t gPreRollCount = 0;

void measureFrame(const int16_t *samples, size_t count, float *outRms, int *outPeak)
{
    int64_t sumSq = 0;
    int peak = 0;
    for (size_t i = 0; i < count; i++)
    {
        const int16_t s = samples[i];
        const int absS = (s >= 0) ? s : -s;
        if (absS > peak)
        {
            peak = absS;
        }
        sumSq += static_cast<int64_t>(s) * s;
    }
    *outRms = (count > 0)
                  ? std::sqrt(static_cast<float>(sumSq) / static_cast<float>(count))
                  : 0.0f;
    *outPeak = peak;
}

bool aboveThreshold(float rms, int peak, float rmsThresh, int peakThresh)
{
    return rms >= rmsThresh || peak >= peakThresh;
}

void appendSamples(const int16_t *samples, size_t count)
{
    const size_t remaining = gMaxSamples - gRecordedSamples;
    const size_t toCopy = (count < remaining) ? count : remaining;
    if (toCopy == 0)
    {
        return;
    }
    int16_t *dst = reinterpret_cast<int16_t *>(gWavBuffer + wav::kHeaderSize) + gRecordedSamples;
    memcpy(dst, samples, toCopy * sizeof(int16_t));
    gRecordedSamples += toCopy;
}

void pushPreRoll(const int16_t *samples)
{
    if (gPreRoll == nullptr || gPreRollFrames == 0)
    {
        return;
    }
    memcpy(
        gPreRoll + gPreRollWrite * audio::kFrameSamples, samples,
        audio::kFrameSamples * sizeof(int16_t));
    gPreRollWrite = (gPreRollWrite + 1) % gPreRollFrames;
    if (gPreRollCount < gPreRollFrames)
    {
        gPreRollCount++;
    }
}

void appendPreRoll()
{
    if (gPreRoll == nullptr || gPreRollCount == 0)
    {
        return;
    }
    const size_t start =
        (gPreRollWrite + gPreRollFrames - gPreRollCount) % gPreRollFrames;
    for (size_t i = 0; i < gPreRollCount; i++)
    {
        const size_t idx = (start + i) % gPreRollFrames;
        appendSamples(gPreRoll + idx * audio::kFrameSamples, audio::kFrameSamples);
        gRecordedMs += kFrameDurationMs;
    }
    gPreRollCount = 0;
    gPreRollWrite = 0;
}

void finishUtterance()
{
    wav::writeHeader(
        gWavBuffer,
        audio::kSampleRateHz,
        audio::kSampleBits,
        /*numChannels=*/1,
        static_cast<uint32_t>(gRecordedSamples * sizeof(int16_t)));
}

} // namespace

bool begin(const Config &config)
{
    gConfig = config;
    gMaxSamples = (static_cast<size_t>(audio::kSampleRateHz) * gConfig.maxRecordingMs) / 1000;
    const size_t bufLen = wav::kHeaderSize + gMaxSamples * sizeof(int16_t);

    gWavBuffer = static_cast<uint8_t *>(heap_caps_malloc(bufLen, MALLOC_CAP_SPIRAM));
    if (gWavBuffer == nullptr)
    {
        return false;
    }

    gPreRollFrames =
        (static_cast<size_t>(gConfig.preRollMs) + kFrameDurationMs - 1) / kFrameDurationMs;
    if (gPreRollFrames < 1)
    {
        gPreRollFrames = 1;
    }
    gPreRoll = static_cast<int16_t *>(heap_caps_malloc(
        gPreRollFrames * audio::kFrameSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    gPreRollWrite = 0;
    gPreRollCount = 0;
    return gPreRoll != nullptr;
}

void calibrate(uint32_t durationMs)
{
    // Discard the first moments of audio: PDM mics commonly produce a loud
    // startup click/pop right after I2S begins, which would otherwise make
    // this one-off transient look like the permanent noise floor and push
    // the speech thresholds absurdly high.
    {
        const uint32_t warmupStart = millis();
        while (millis() - warmupStart < kCalibrationWarmupMs)
        {
            int16_t frame[audio::kFrameSamples];
            audio::readFrame(frame, 50);
        }
    }

    Serial.println("[vad] калибровка по тишине, не говорите…");

    float sumRms = 0.0f;
    int64_t sumPeak = 0;
    float maxRms = 0.0f;
    int maxPeak = 0;
    uint32_t framesSeen = 0;

    const uint32_t start = millis();
    while (millis() - start < durationMs)
    {
        int16_t frame[audio::kFrameSamples];
        if (!audio::readFrame(frame, 50))
        {
            continue;
        }

        float rms = 0.0f;
        int peak = 0;
        measureFrame(frame, audio::kFrameSamples, &rms, &peak);
        framesSeen++;

        sumRms += rms;
        sumPeak += peak;
        maxRms = std::max(maxRms, rms);
        maxPeak = std::max(maxPeak, peak);
    }

    if (framesSeen == 0)
    {
        Serial.println("[vad] калибровка не удалась (нет кадров), использую пороги по умолчанию");
        return;
    }

    // Base the noise floor on the *average* level rather than the single
    // loudest frame: one stray click during calibration would otherwise
    // permanently inflate the thresholds and make real speech unable to
    // cross them (the exact failure mode this replaces).
    const float avgRms = sumRms / static_cast<float>(framesSeen);
    const int avgPeak = static_cast<int>(sumPeak / static_cast<int64_t>(framesSeen));

    // Raise thresholds above the average noise with margin, but never below
    // the defaults passed to begin() -- those remain a sane floor for an
    // environment that happens to be truly silent during calibration.
    const float startRms = std::max(gConfig.startRms, avgRms * 4.0f + 150.0f);
    const int startPeak = std::max(gConfig.startPeak, static_cast<int>(avgPeak * 3.0f) + 400);

    // Keep continue thresholds safely below the start thresholds so a
    // genuine utterance isn't cut short mid-sentence by a brief dip.
    float continueRms = std::max(gConfig.continueRms, avgRms * 2.5f + 80.0f);
    int continuePeak = std::max(gConfig.continuePeak, static_cast<int>(avgPeak * 2.0f) + 200);
    continueRms = std::min(continueRms, startRms * 0.75f);
    continuePeak = std::min(continuePeak, static_cast<int>(startPeak * 0.75f));

    gConfig.startRms = startRms;
    gConfig.startPeak = startPeak;
    gConfig.continueRms = continueRms;
    gConfig.continuePeak = continuePeak;

    Serial.printf(
        "[vad] шумовой пол: средн. RMS %.0f (макс %.0f), средн. пик %d (макс %d) -> "
        "пороги старт RMS %.0f/пик %d, продолжение RMS %.0f/пик %d\n",
        avgRms, maxRms, avgPeak, maxPeak, gConfig.startRms, gConfig.startPeak,
        gConfig.continueRms, gConfig.continuePeak);
}

Event poll()
{
    int16_t frame[audio::kFrameSamples];
    if (!audio::readFrame(frame, 50))
    {
        return Event::None;
    }

    float rms = 0.0f;
    int peak = 0;
    measureFrame(frame, audio::kFrameSamples, &rms, &peak);

    if (gState == State::Idle)
    {
        pushPreRoll(frame);

        if (aboveThreshold(rms, peak, gConfig.startRms, gConfig.startPeak))
        {
            gStartDebounce++;
            if (gStartDebounce >= kStartDebounceFrames)
            {
                gState = State::Recording;
                gRecordedSamples = 0;
                gRecordedMs = 0;
                gSilenceMs = 0;
                gStartDebounce = 0;
                gLastStatusMs = millis();
                appendPreRoll();
                Serial.println("[vad] слышу речь, записываю…");
            }
        }
        else
        {
            gStartDebounce = 0;

            if (kVadVerbose)
            {
                const uint32_t now = millis();
                if (now - gLastIdleStatusMs >= kIdleStatusIntervalMs)
                {
                    gLastIdleStatusMs = now;
                    Serial.printf(
                        "[vad] жду речь: rms=%.0f (порог %.0f), peak=%d (порог %d)\n",
                        rms, gConfig.startRms, peak, gConfig.startPeak);
                }
            }
        }
        return Event::None;
    }

    // State::Recording
    appendSamples(frame, audio::kFrameSamples);
    gRecordedMs += kFrameDurationMs;

    if (aboveThreshold(rms, peak, gConfig.continueRms, gConfig.continuePeak))
    {
        gSilenceMs = 0;
    }
    else
    {
        gSilenceMs += kFrameDurationMs;
    }

    if (kVadVerbose)
    {
        const uint32_t now = millis();
        if (now - gLastStatusMs >= kStatusIntervalMs)
        {
            gLastStatusMs = now;
            Serial.printf(
                "[vad] запись: %.1fс (тишина %.1f/%.1fс), rms=%.0f peak=%d\n",
                gRecordedMs / 1000.0f,
                gSilenceMs / 1000.0f,
                gConfig.endSilenceMs / 1000.0f,
                rms,
                peak);
        }
    }

    const bool bufferFull = gRecordedSamples >= gMaxSamples;
    const bool longEnoughSilence =
        gSilenceMs >= gConfig.endSilenceMs && gRecordedMs >= gConfig.minUtteranceMs;

    if (bufferFull || longEnoughSilence)
    {
        if (bufferFull)
        {
            Serial.println("[vad] достигнут лимит длительности записи (20с), останавливаю запись");
        }
        else
        {
            Serial.printf(
                "[vad] пауза в речи обнаружена, запись окончена (длительность %.1fс)\n",
                gRecordedMs / 1000.0f);
        }
        finishUtterance();
        gState = State::Idle;
        return Event::UtteranceReady;
    }

    return Event::None;
}

bool isRecording()
{
    return gState == State::Recording;
}

const uint8_t *wavData()
{
    return gWavBuffer;
}

size_t wavLen()
{
    return wav::kHeaderSize + gRecordedSamples * sizeof(int16_t);
}

void reset()
{
    gRecordedSamples = 0;
    gRecordedMs = 0;
    gSilenceMs = 0;
    gStartDebounce = 0;
    gPreRollWrite = 0;
    gPreRollCount = 0;
    gState = State::Idle;
}

} // namespace vad
