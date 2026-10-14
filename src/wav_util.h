#pragma once

#include <cstddef>
#include <cstdint>

/** Minimal canonical PCM WAV header writer. */
namespace wav
{

constexpr size_t kHeaderSize = 44;

/**
 * Writes a 44-byte canonical PCM WAV header into buf[0..kHeaderSize-1].
 * dataBytes is the size of the PCM payload that follows the header.
 */
void writeHeader(
    uint8_t *buf,
    uint32_t sampleRate,
    uint16_t bitsPerSample,
    uint16_t numChannels,
    uint32_t dataBytes);

} // namespace wav
