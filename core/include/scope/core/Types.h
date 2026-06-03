#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <chrono>

namespace scope::core {

// Timestamp in nanoseconds since UNIX epoch. 64-bit signed is enough for ~292 y
// past 1970 and matches std::chrono::nanoseconds.
using TimestampNs = std::int64_t;

inline TimestampNs nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Subset of ADS / IEC 61131-3 scalar types we record. Strings and structs are
// out of scope for v1.
enum class DataType : std::uint8_t {
    Bool,
    Int8,    Uint8,
    Int16,   Uint16,
    Int32,   Uint32,
    Int64,   Uint64,
    Float32, Float64,
};

constexpr std::size_t sizeOf(DataType t) noexcept {
    switch (t) {
        case DataType::Bool:    return 1;
        case DataType::Int8:    return 1;
        case DataType::Uint8:   return 1;
        case DataType::Int16:   return 2;
        case DataType::Uint16:  return 2;
        case DataType::Int32:   return 4;
        case DataType::Uint32:  return 4;
        case DataType::Int64:   return 8;
        case DataType::Uint64:  return 8;
        case DataType::Float32: return 4;
        case DataType::Float64: return 8;
    }
    return 0;
}

const char* toString(DataType t) noexcept;

// Acquisition mode for a Recorder channel. See _ClaudeMemory/sampling_model.md.
enum class AcquisitionMode : std::uint8_t {
    AdsNotify,    // ADS device notification at parent task cycle
    Oversampled,  // ARRAY[0..N-1] read in chunks from an oversampling source
};

const char* toString(AcquisitionMode m) noexcept;

}  // namespace scope::core
