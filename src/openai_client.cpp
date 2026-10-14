#include "openai_client.h"

#include "audio_capture.h"
#include "openai_realtime.h"
#include "wav_util.h"

#include <Arduino.h>

namespace openai
{

Result askAudio(const Config &config, const uint8_t *wavBytes, size_t wavLen)
{
    Result result;

    if (wavBytes == nullptr || wavLen <= wav::kHeaderSize)
    {
        result.error = "Пустая запись";
        return result;
    }

    const int16_t *pcm16 = reinterpret_cast<const int16_t *>(wavBytes + wav::kHeaderSize);
    const size_t sampleCount16 = (wavLen - wav::kHeaderSize) / sizeof(int16_t);

    const openai_realtime::Config rtCfg{
        config.apiKey,
        config.realtimeModel,
        config.responsesModel,
        config.systemPrompt,
        config.voice,
        config.reasoningEffort,
    };

    // openai_realtime resamples internally, one small chunk at a time, so no
    // second full-length PSRAM copy of the utterance is allocated here.
    const openai_realtime::Result rt =
        openai_realtime::askUtterance(rtCfg, pcm16, sampleCount16, audio::kSampleRateHz);

    result.ok = rt.ok;
    result.usedWebSearch = rt.usedWebSearch;
    result.text = rt.text;
    result.citations = rt.citations;
    result.error = rt.error;
    return result;
}

} // namespace openai
