#include "scope/core/Mdf4Session.h"

#include <mdf/mdffactory.h>
#include <mdf/mdfwriter.h>
#include <mdf/mdfreader.h>
#include <mdf/mdffile.h>
#include <mdf/iheader.h>
#include <mdf/imetadata.h>
#include <mdf/idatagroup.h>
#include <mdf/ichannelgroup.h>
#include <mdf/ichannel.h>
#include <mdf/ichannelobserver.h>

#include <spdlog/spdlog.h>

#include <QHash>
#include <QStringList>

#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace scope::core {

namespace {

// Pack a few extra meta fields into the channel description as
// "key1=value1;key2=value2;…". Round-trips through arbitrary MDF tools
// (the description field is plain text per spec) and survives if the file
// is rewritten by another writer that respects descriptions.
QString packDescription(const Signal::Meta& m) {
    QString s;
    s += "domain=";
    s += (m.domain == Signal::Domain::Frequency ? "frequency" : "time");
    s += ";source_symbol=";
    s += m.sourceSymbol;
    s += ";data_type=";
    s += QString::fromUtf8(toString(m.dataType));
    if (m.sampleRateHz > 0.0)
        s += ";sample_rate_hz=" + QString::number(m.sampleRateHz, 'g', 15);
    if (m.parentTaskCycleUs > 0)
        s += ";parent_task_cycle_us=" + QString::number(m.parentTaskCycleUs);
    return s;
}

void unpackDescription(const std::string& descUtf8, Signal::Meta& m) {
    const QString desc = QString::fromStdString(descUtf8);
    for (const QString& kv : desc.split(';', Qt::SkipEmptyParts)) {
        const int eq = kv.indexOf('=');
        if (eq < 0) continue;
        const QString k = kv.left(eq);
        const QString v = kv.mid(eq + 1);
        if (k == "domain") {
            m.domain = (v == "frequency") ? Signal::Domain::Frequency
                                          : Signal::Domain::Time;
        } else if (k == "source_symbol") {
            m.sourceSymbol = v;
        } else if (k == "data_type") {
            static const std::pair<const char*, DataType> kMap[] = {
                {"BOOL", DataType::Bool},
                {"SINT", DataType::Int8},   {"USINT", DataType::Uint8},
                {"INT", DataType::Int16},   {"UINT", DataType::Uint16},
                {"DINT", DataType::Int32},  {"UDINT", DataType::Uint32},
                {"LINT", DataType::Int64},  {"ULINT", DataType::Uint64},
                {"REAL", DataType::Float32}, {"LREAL", DataType::Float64},
            };
            const std::string vs = v.toStdString();
            for (const auto& [k2, v2] : kMap)
                if (vs == k2) { m.dataType = v2; break; }
        } else if (k == "sample_rate_hz") {
            m.sampleRateHz = v.toDouble();
        } else if (k == "parent_task_cycle_us") {
            m.parentTaskCycleUs = static_cast<std::uint32_t>(v.toUInt());
        }
    }
}

// Convert one raw byte sample to double given its scope DataType.
double rawToDouble(const std::byte* p, DataType t) {
    auto get = [&](auto tag) {
        using T = decltype(tag);
        T v;
        std::memcpy(&v, p, sizeof(T));
        return static_cast<double>(v);
    };
    switch (t) {
        case DataType::Bool:    return get(std::uint8_t{});
        case DataType::Int8:    return get(std::int8_t{});
        case DataType::Uint8:   return get(std::uint8_t{});
        case DataType::Int16:   return get(std::int16_t{});
        case DataType::Uint16:  return get(std::uint16_t{});
        case DataType::Int32:   return get(std::int32_t{});
        case DataType::Uint32:  return get(std::uint32_t{});
        case DataType::Int64:   return get(std::int64_t{});
        case DataType::Uint64:  return get(std::uint64_t{});
        case DataType::Float32: return get(float{});
        case DataType::Float64: return get(double{});
    }
    return 0.0;
}

}  // namespace

struct Mdf4Session::Impl {
    std::unique_ptr<mdf::MdfWriter> writer;
    std::unique_ptr<mdf::MdfReader> reader;
    std::filesystem::path path;
    bool writable{false};
    bool measurementStarted{false};
    bool finalized{false};

    struct ChannelState {
        Signal::Meta meta;
        mdf::IDataGroup*    dg = nullptr;
        mdf::IChannelGroup* cg = nullptr;
        mdf::IChannel*      value = nullptr;
    };
    std::unordered_map<QString, ChannelState> channels;
};

std::unique_ptr<Mdf4Session> Mdf4Session::create(const std::filesystem::path& path,
                                                 QString* errorOut) {
    try {
        auto s = std::unique_ptr<Mdf4Session>(new Mdf4Session());
        s->impl_->writer = mdf::MdfFactory::CreateMdfWriter(mdf::MdfWriterType::Mdf4Basic);
        if (!s->impl_->writer) {
            if (errorOut) *errorOut = "MdfFactory returned null writer";
            return nullptr;
        }
        if (!s->impl_->writer->Init(path.string())) {
            if (errorOut) *errorOut = "MdfWriter::Init failed";
            return nullptr;
        }
        s->impl_->writable = true;
        s->impl_->path = path;
        if (auto* h = s->impl_->writer->Header()) {
            h->Author("ScopeAnalyser");
            h->Description("ScopeAnalyser session");
        }
        return s;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Mdf4Session::create({}): {}", path.string(), e.what());
        return nullptr;
    }
}

std::unique_ptr<Mdf4Session> Mdf4Session::openForRead(const std::filesystem::path& path,
                                                      QString* errorOut) {
    try {
        auto s = std::unique_ptr<Mdf4Session>(new Mdf4Session());
        s->impl_->reader = std::make_unique<mdf::MdfReader>(path.string());
        if (!s->impl_->reader->IsOk()) {
            if (errorOut) *errorOut = "MdfReader could not open file";
            return nullptr;
        }
        s->impl_->writable = false;
        s->impl_->path = path;
        return s;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Mdf4Session::openForRead({}): {}", path.string(), e.what());
        return nullptr;
    }
}

Mdf4Session::Mdf4Session() : impl_(std::make_unique<Impl>()) {}
Mdf4Session::~Mdf4Session() {
    // Best-effort finalize if the caller forgot. Errors are logged, not thrown.
    if (impl_->writable && !impl_->finalized) {
        QString err;
        if (!finalize(&err) && !err.isEmpty())
            spdlog::warn("Mdf4Session dtor: finalize failed: {}", err.toStdString());
    }
}

bool Mdf4Session::addChannel(const Signal::Meta& meta, QString* errorOut) {
    if (!impl_->writable) {
        if (errorOut) *errorOut = "File opened read-only";
        return false;
    }
    if (impl_->measurementStarted) {
        if (errorOut) *errorOut = "Cannot add channels after appendSamples has been called";
        return false;
    }
    try {
        auto* dg = impl_->writer->CreateDataGroup();
        auto* cg = mdf::MdfWriter::CreateChannelGroup(dg);
        cg->Name(meta.name.toStdString());

        auto* master = mdf::MdfWriter::CreateChannel(cg);
        master->Name("X");
        master->Type(mdf::ChannelType::Master);
        master->Sync(mdf::ChannelSyncType::Time);
        master->DataType(mdf::ChannelDataType::FloatLe);
        master->DataBytes(8);
        master->Unit(meta.domain == Signal::Domain::Frequency ? "Hz" : "s");

        auto* val = mdf::MdfWriter::CreateChannel(cg);
        val->Name(meta.name.toStdString());
        val->Type(mdf::ChannelType::FixedLength);
        val->DataType(mdf::ChannelDataType::FloatLe);
        val->DataBytes(8);
        val->Unit(meta.unit.toStdString());
        val->Description(packDescription(meta).toStdString());

        impl_->channels.emplace(meta.name, Impl::ChannelState{meta, dg, cg, val});
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Mdf4Session::addChannel({}): {}",
                      meta.name.toStdString(), e.what());
        return false;
    }
}

bool Mdf4Session::appendSamples(const QString& channelName,
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
    try {
        if (!impl_->measurementStarted) {
            impl_->writer->InitMeasurement();
            // Use start_time = 1 ns (not 0) so mdflib's SampleQueue::TrimQueue
            // doesn't aggressively prune all-but-one queued samples. With
            // start_time == 0 it falls into a branch that treats "any positive
            // buffer time" as "still pre-trig" and drops the older samples;
            // with a small positive start_time the branch instead checks
            // `next_time >= start_time` which is true for every typical
            // timestamp we hand it, so the queue is preserved verbatim. The
            // 1ns offset is removed on read by adding Header()->StartTime()
            // back to each master value.
            impl_->writer->StartMeasurement(static_cast<uint64_t>(1));
            impl_->measurementStarted = true;
        }
        auto& ch = it->second;
        const std::size_t bps = sizeOf(ch.meta.dataType);
        for (std::size_t i = 0; i < count; ++i) {
            const double v = rawToDouble(values + i * bps, ch.meta.dataType);
            ch.value->SetChannelValue(v);
            const uint64_t ts = static_cast<uint64_t>(timestamps[i]);
            impl_->writer->SaveSample(*ch.cg, ts);
        }
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Mdf4Session::appendSamples({}, n={}): {}",
                      channelName.toStdString(), count, e.what());
        return false;
    }
}

bool Mdf4Session::finalize(QString* errorOut) {
    if (!impl_->writable || impl_->finalized) return true;
    try {
        if (impl_->measurementStarted) {
            impl_->writer->StopMeasurement(static_cast<uint64_t>(0));
        } else {
            // No samples were ever appended. Still need InitMeasurement
            // before FinalizeMeasurement to write a valid (empty) file.
            impl_->writer->InitMeasurement();
        }
        impl_->writer->FinalizeMeasurement();
        impl_->finalized = true;
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Mdf4Session::finalize: {}", e.what());
        return false;
    }
}

std::vector<std::shared_ptr<Signal>> Mdf4Session::loadAllSignals(QString* errorOut) {
    std::vector<std::shared_ptr<Signal>> out;
    if (!impl_->reader) {
        if (errorOut) *errorOut = "Not opened for read";
        return out;
    }
    try {
        impl_->reader->ReadEverythingButData();
        const auto* file = impl_->reader->GetFile();
        if (!file) {
            if (errorOut) *errorOut = "Reader has no MdfFile instance";
            return out;
        }
        // We pass start_time=1 on write so the writer's pre-trig queue keeps
        // all samples. The master column then stores (sample_ts - 1) / 1e9
        // seconds, which we reverse here by adding Header::StartTime() back.
        const auto* header = file->Header();
        const uint64_t fileStartNs = header ? header->StartTime() : 0;

        mdf::DataGroupList dgList;
        file->DataGroups(dgList);

        for (auto* dg : dgList) {
            const auto cgList = dg->ChannelGroups();
            for (const auto* cg : cgList) {
                const auto cnList = cg->Channels();
                const mdf::IChannel* master = nullptr;
                const mdf::IChannel* val    = nullptr;
                for (const auto* cn : cnList) {
                    if (cn->Type() == mdf::ChannelType::Master
                        || cn->Type() == mdf::ChannelType::VirtualMaster) {
                        master = cn;
                    } else if (cn->IsNumber()) {
                        val = cn;
                    }
                }
                if (!master || !val) continue;

                auto masterObs = mdf::CreateChannelObserver(*dg, *cg, *master);
                auto valObs    = mdf::CreateChannelObserver(*dg, *cg, *val);
                impl_->reader->ReadData(*dg);

                Signal::Meta meta;
                meta.name = QString::fromStdString(val->Name());
                meta.unit = QString::fromStdString(val->Unit());
                unpackDescription(val->Description(), meta);
                // We always promote values to Float64 on write, so the in-memory
                // Signal stores Float64 regardless of the original dataType.
                // (The original type is preserved in meta.dataType from the
                // description for informational use; callers must not interpret
                // the byte buffer through that type.)
                meta.dataType = DataType::Float64;

                const std::size_t n = static_cast<std::size_t>(valObs->NofSamples());
                std::vector<TimestampNs> ts(n);
                std::vector<double>      vs(n);
                for (std::size_t i = 0; i < n; ++i) {
                    double tSec = 0.0;
                    masterObs->GetChannelValue(i, tSec);
                    // Add the file's StartTime back so the value lines up
                    // with what was originally passed to appendSamples().
                    ts[i] = static_cast<TimestampNs>(fileStartNs)
                          + static_cast<TimestampNs>(std::llround(tSec * 1e9));
                    double v = 0.0;
                    valObs->GetEngValue(i, v);
                    vs[i] = v;
                }
                auto sig = std::make_shared<Signal>(meta);
                sig->append(ts.data(),
                            reinterpret_cast<const std::byte*>(vs.data()),
                            n);
                out.push_back(std::move(sig));

                dg->ClearData();
            }
        }
        return out;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Mdf4Session::loadAllSignals: {}", e.what());
        return {};
    }
}

bool Mdf4Session::writeLayout(const QString& json, QString* errorOut) {
    if (!impl_->writable || !impl_->writer) {
        if (errorOut) *errorOut = "File not open for write";
        return false;
    }
    if (json.isEmpty()) return true;  // nothing to embed
    try {
        auto* header = impl_->writer->Header();
        if (!header) {
            if (errorOut) *errorOut = "Writer has no header";
            return false;
        }
        auto* md = header->MetaData();
        if (!md) md = header->CreateMetaData();
        if (!md) {
            if (errorOut) *errorOut = "Couldn't create header metadata";
            return false;
        }
        md->StringProperty("plot_layout", json.toStdString());
        return true;
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = QString::fromUtf8(e.what());
        spdlog::error("Mdf4Session::writeLayout: {}", e.what());
        return false;
    }
}

QString Mdf4Session::readLayout() const {
    if (!impl_->reader) return {};
    try {
        impl_->reader->ReadEverythingButData();  // ensure HD/MD blocks are in
        const auto* file = impl_->reader->GetFile();
        if (!file) return {};
        const auto* header = file->Header();
        if (!header) return {};
        const auto* md = header->MetaData();
        if (!md) return {};
        return QString::fromStdString(md->StringProperty("plot_layout"));
    } catch (const std::exception& e) {
        spdlog::warn("Mdf4Session::readLayout: {}", e.what());
        return {};
    }
}

}  // namespace scope::core
