#include "scope/core/Signal.h"

#include <cstring>

namespace scope::core {

const char* toString(DataType t) noexcept {
    switch (t) {
        case DataType::Bool:    return "BOOL";
        case DataType::Int8:    return "SINT";
        case DataType::Uint8:   return "USINT";
        case DataType::Int16:   return "INT";
        case DataType::Uint16:  return "UINT";
        case DataType::Int32:   return "DINT";
        case DataType::Uint32:  return "UDINT";
        case DataType::Int64:   return "LINT";
        case DataType::Uint64:  return "ULINT";
        case DataType::Float32: return "REAL";
        case DataType::Float64: return "LREAL";
    }
    return "UNKNOWN";
}

const char* toString(AcquisitionMode m) noexcept {
    switch (m) {
        case AcquisitionMode::AdsNotify:   return "AdsNotify";
        case AcquisitionMode::Oversampled: return "Oversampled";
    }
    return "Unknown";
}

Signal::Signal(Meta meta) : meta_(std::move(meta)) {}

void Signal::setMeta(Meta meta) {
    std::lock_guard lock(mtx_);
    meta_ = std::move(meta);
}

void Signal::append(const TimestampNs* timestamps,
                    const std::byte* valueBytes,
                    std::size_t count) {
    if (count == 0) return;
    const std::size_t bps = bytesPerSample();
    std::lock_guard lock(mtx_);
    const std::size_t oldSize = timestamps_.size();
    timestamps_.resize(oldSize + count);
    std::memcpy(timestamps_.data() + oldSize, timestamps, count * sizeof(TimestampNs));
    const std::size_t oldBytes = values_.size();
    values_.resize(oldBytes + count * bps);
    std::memcpy(values_.data() + oldBytes, valueBytes, count * bps);
    sampleCount_.store(timestamps_.size(), std::memory_order_release);
}

std::size_t Signal::sampleCount() const {
    return sampleCount_.load(std::memory_order_acquire);
}

TimestampNs Signal::displayOriginNs() const {
    std::lock_guard lock(mtx_);
    if (meta_.sourceStartNs != kNoSourceStart) return meta_.sourceStartNs;
    return timestamps_.empty() ? 0 : timestamps_.front();
}

Signal::ReadView Signal::snapshotForRead() const {
    std::lock_guard lock(mtx_);
    return ReadView{
        timestamps_.data(),
        values_.data(),
        timestamps_.size(),
        meta_.dataType
    };
}

namespace {
template <typename T>
double toDoubleAt(const std::byte* base, std::size_t i) {
    T v;
    std::memcpy(&v, base + i * sizeof(T), sizeof(T));
    return static_cast<double>(v);
}
}  // namespace

std::vector<double> Signal::readAsDouble() const {
    std::lock_guard lock(mtx_);
    std::vector<double> out;
    out.reserve(timestamps_.size());
    const std::byte* base = values_.data();
    const std::size_t n = timestamps_.size();
    switch (meta_.dataType) {
        case DataType::Bool:
        case DataType::Uint8:   for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<std::uint8_t>(base, i));  break;
        case DataType::Int8:    for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<std::int8_t>(base, i));   break;
        case DataType::Int16:   for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<std::int16_t>(base, i));  break;
        case DataType::Uint16:  for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<std::uint16_t>(base, i)); break;
        case DataType::Int32:   for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<std::int32_t>(base, i));  break;
        case DataType::Uint32:  for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<std::uint32_t>(base, i)); break;
        case DataType::Int64:   for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<std::int64_t>(base, i));  break;
        case DataType::Uint64:  for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<std::uint64_t>(base, i)); break;
        case DataType::Float32: for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<float>(base, i));         break;
        case DataType::Float64: for (std::size_t i = 0; i < n; ++i) out.push_back(toDoubleAt<double>(base, i));        break;
    }
    return out;
}

}  // namespace scope::core
