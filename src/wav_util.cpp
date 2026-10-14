#include "wav_util.h"

#include <cstring>

namespace wav
{

namespace
{

void writeU32LE(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void writeU16LE(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

} // namespace

void writeHeader(
    uint8_t *buf,
    uint32_t sampleRate,
    uint16_t bitsPerSample,
    uint16_t numChannels,
    uint32_t dataBytes)
{
    const uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    const uint16_t blockAlign = static_cast<uint16_t>(numChannels * (bitsPerSample / 8));
    const uint32_t riffChunkSize = 36 + dataBytes;

    memcpy(buf + 0, "RIFF", 4);
    writeU32LE(buf + 4, riffChunkSize);
    memcpy(buf + 8, "WAVE", 4);

    memcpy(buf + 12, "fmt ", 4);
    writeU32LE(buf + 16, 16); // PCM fmt chunk size
    writeU16LE(buf + 20, 1);  // audio format = PCM
    writeU16LE(buf + 22, numChannels);
    writeU32LE(buf + 24, sampleRate);
    writeU32LE(buf + 28, byteRate);
    writeU16LE(buf + 32, blockAlign);
    writeU16LE(buf + 34, bitsPerSample);

    memcpy(buf + 36, "data", 4);
    writeU32LE(buf + 40, dataBytes);
}

} // namespace wav
