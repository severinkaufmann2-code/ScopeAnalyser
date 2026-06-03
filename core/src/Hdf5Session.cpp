#include "scope/core/Hdf5Session.h"

#include <highfive/H5File.hpp>
#include <highfive/H5DataSet.hpp>
#include <highfive/H5DataSpace.hpp>
#include <highfive/H5PropertyList.hpp>
#include <highfive/H5Attribute.hpp>

#include <spdlog/spdlog.h>

#include <unordered_map>

namespace scope::core {

namespace {

constexpr std::size_t kChunkSamples = 8192;  // ~1s @ 8 kHz; HDF5 likes ~1MB chunks
constexpr std::uint32_t kCompressionLevel = 4;

HighFive::DataType hdf5TypeFor(DataType t) {
    using HighFive::AtomicType;
    switch (t) {
        case DataType::Bool:    return AtomicType<std::uint8_t>();
        case DataType::Int8:    return AtomicType<std::int8_t>();
        case DataType::Uint8:   return AtomicType<std::uint8_t>();
        case DataType::Int16:   return AtomicType<std::int16_t>();
        case DataType::Uint16:  return AtomicType<std::uint16_t>();
        case DataType::Int32:   return AtomicType<std::int32_t>();
        case DataType::Uint32:  return AtomicType<std::uint32_t>();
        case DataType::Int64:   return AtomicType<std::int64_t>();
        case DataType::Uint64:  return AtomicType<std::uint64_t>();
        case DataType::Float32: return AtomicType<float>();
        case DataType::Float64: return AtomicType<double>();
    }
    return AtomicType<double>();
}

}  // namespace

struct Hdf5Session::Impl {
    std::unique_ptr<HighFive::File> file;
    bool writable{false};

    struct ChannelState {
        Signal::Meta meta;
        HighFive::DataSet timestamps;
        HighFive::DataSet values;
        std::size_t written{0};
    };
    std::unordered_map<QString, ChannelState> channels;

    HighFive::Group ensureChannelGroup(const QString& name) {
        const std::string p = "/channels/" + name.toStdString();
        if (file->exist(p)) return file->getGroup(p);
        return file->createGroup(p);
    }
};

std::unique_ptr<Hdf5Session> Hdf5Session::create(const std::filesystem::path& path,
                                                 QString* errorOut) {
    try {
        auto s = std::unique_ptr<Hdf5Session>(new Hdf5Session());
        s->impl_->file = std::make_unique<HighFive::File>(
            path.string(),
            HighFive::File::Overwrite);
        s->impl_->writable = true;
        s->impl_->file->createGroup("/channels");
        return s;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Hdf5Session::create({}): {}", path.string(), e.what());
        return nullptr;
    }
}

std::unique_ptr<Hdf5Session> Hdf5Session::openForRead(const std::filesystem::path& path,
                                                      QString* errorOut) {
    try {
        auto s = std::unique_ptr<Hdf5Session>(new Hdf5Session());
        s->impl_->file = std::make_unique<HighFive::File>(
            path.string(),
            HighFive::File::ReadOnly);
        s->impl_->writable = false;
        return s;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Hdf5Session::openForRead({}): {}", path.string(), e.what());
        return nullptr;
    }
}

Hdf5Session::Hdf5Session() : impl_(std::make_unique<Impl>()) {}
Hdf5Session::~Hdf5Session() = default;

bool Hdf5Session::addChannel(const Signal::Meta& meta, QString* errorOut) {
    if (!impl_->writable) {
        if (errorOut) *errorOut = "File opened read-only";
        return false;
    }
    try {
        auto group = impl_->ensureChannelGroup(meta.name);

        HighFive::DataSpace tsSpace{{0}, {HighFive::DataSpace::UNLIMITED}};
        HighFive::DataSetCreateProps tsProps;
        tsProps.add(HighFive::Chunking({kChunkSamples}));
        tsProps.add(HighFive::Deflate(kCompressionLevel));

        auto tsDataset = group.createDataSet<std::int64_t>("timestamps", tsSpace, tsProps);

        HighFive::DataSpace valSpace{{0}, {HighFive::DataSpace::UNLIMITED}};
        HighFive::DataSetCreateProps valProps;
        valProps.add(HighFive::Chunking({kChunkSamples}));
        valProps.add(HighFive::Deflate(kCompressionLevel));

        auto valDataset = group.createDataSet("values", valSpace, hdf5TypeFor(meta.dataType), valProps);

        valDataset.createAttribute<std::string>("unit", meta.unit.toStdString());
        valDataset.createAttribute<std::string>("source_symbol", meta.sourceSymbol.toStdString());
        valDataset.createAttribute<std::string>("mode", std::string(toString(meta.mode)));
        valDataset.createAttribute<double>("sample_rate_hz", meta.sampleRateHz);
        valDataset.createAttribute<std::uint32_t>("parent_task_cycle_us", meta.parentTaskCycleUs);
        valDataset.createAttribute<std::string>("data_type", std::string(toString(meta.dataType)));

        impl_->channels.emplace(meta.name, Impl::ChannelState{
            meta, std::move(tsDataset), std::move(valDataset), 0});
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Hdf5Session::addChannel({}): {}", meta.name.toStdString(), e.what());
        return false;
    }
}

bool Hdf5Session::appendSamples(const QString& channelName,
                                const TimestampNs* timestamps,
                                const std::byte* values,
                                std::size_t count,
                                QString* errorOut) {
    if (count == 0) return true;
    auto it = impl_->channels.find(channelName);
    if (it == impl_->channels.end()) {
        if (errorOut) *errorOut = "Unknown channel: " + channelName;
        return false;
    }
    auto& ch = it->second;
    try {
        const std::size_t oldSize = ch.written;
        const std::size_t newSize = oldSize + count;

        ch.timestamps.resize({newSize});
        ch.timestamps.select({oldSize}, {count})
            .write_raw(timestamps);

        ch.values.resize({newSize});
        ch.values.select({oldSize}, {count})
            .write_raw(reinterpret_cast<const void*>(values),
                       hdf5TypeFor(ch.meta.dataType));

        ch.written = newSize;
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Hdf5Session::appendSamples({}, n={}): {}",
                      channelName.toStdString(), count, e.what());
        return false;
    }
}

std::vector<std::shared_ptr<Signal>> Hdf5Session::loadAllSignals(QString* errorOut) {
    std::vector<std::shared_ptr<Signal>> out;
    try {
        if (!impl_->file->exist("/channels")) return out;
        auto channels = impl_->file->getGroup("/channels");
        for (const auto& name : channels.listObjectNames()) {
            auto group = channels.getGroup(name);
            auto valDs = group.getDataSet("values");
            auto tsDs = group.getDataSet("timestamps");

            Signal::Meta meta;
            meta.name = QString::fromStdString(name);
            meta.unit = QString::fromStdString(
                valDs.getAttribute("unit").read<std::string>());
            meta.sourceSymbol = QString::fromStdString(
                valDs.getAttribute("source_symbol").read<std::string>());
            meta.sampleRateHz = valDs.getAttribute("sample_rate_hz").read<double>();
            meta.parentTaskCycleUs = valDs.getAttribute("parent_task_cycle_us").read<std::uint32_t>();

            const std::string dt = valDs.getAttribute("data_type").read<std::string>();
            // Map back to DataType. Simple linear table.
            static const std::pair<const char*, DataType> kMap[] = {
                {"BOOL", DataType::Bool},
                {"SINT", DataType::Int8},  {"USINT", DataType::Uint8},
                {"INT", DataType::Int16},  {"UINT", DataType::Uint16},
                {"DINT", DataType::Int32}, {"UDINT", DataType::Uint32},
                {"LINT", DataType::Int64}, {"ULINT", DataType::Uint64},
                {"REAL", DataType::Float32}, {"LREAL", DataType::Float64},
            };
            meta.dataType = DataType::Float64;
            for (const auto& [k, v] : kMap) if (dt == k) { meta.dataType = v; break; }

            auto sig = std::make_shared<Signal>(meta);

            std::vector<TimestampNs> ts;
            tsDs.read(ts);
            std::vector<std::byte> vals(valDs.getElementCount() * sizeOf(meta.dataType));
            valDs.read_raw(vals.data(), hdf5TypeFor(meta.dataType));
            sig->append(ts.data(), vals.data(), ts.size());

            out.push_back(std::move(sig));
        }
        return out;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Hdf5Session::loadAllSignals: {}", e.what());
        return {};
    }
}

void Hdf5Session::flush() {
    if (impl_->file) impl_->file->flush();
}

}  // namespace scope::core
