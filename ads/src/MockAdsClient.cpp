#include "scope/ads/MockAdsClient.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace scope::ads {

using namespace scope::core;

namespace {

// Index group used by all mock symbols. The offset disambiguates per symbol.
constexpr std::uint32_t kMockGroup = 0xDEAD0000;

struct MockSignalDef {
    QString name;
    QString typeName;
    DataType dataType;
    std::uint32_t indexOffset;
    std::uint32_t taskCycleUs;
    // Generator: writes `bytesPerSample` bytes into `out` at sample index i.
    // `t` is seconds since the symbol's generator thread started.
    void (*generate)(std::byte* out, std::uint64_t i, double t);
};

void genSineLREAL(std::byte* out, std::uint64_t, double t) {
    double v = std::sin(2.0 * M_PI * t);
    std::memcpy(out, &v, sizeof(v));
}
void genCosineLREAL(std::byte* out, std::uint64_t, double t) {
    double v = std::cos(20.0 * M_PI * t);
    std::memcpy(out, &v, sizeof(v));
}
void genSawtoothREAL(std::byte* out, std::uint64_t, double t) {
    float v = static_cast<float>(std::fmod(t, 1.0));
    std::memcpy(out, &v, sizeof(v));
}
void genCounterDINT(std::byte* out, std::uint64_t i, double) {
    std::int32_t v = static_cast<std::int32_t>(i);
    std::memcpy(out, &v, sizeof(v));
}
void genToggleBOOL(std::byte* out, std::uint64_t i, double) {
    std::uint8_t v = (i & 1) ? 1 : 0;
    std::memcpy(out, &v, sizeof(v));
}
void genNoisySineLREAL(std::byte* out, std::uint64_t, double t) {
    thread_local std::mt19937 rng{std::random_device{}()};
    thread_local std::normal_distribution<double> noise{0.0, 0.05};
    double v = std::sin(2.0 * M_PI * t) + noise(rng);
    std::memcpy(out, &v, sizeof(v));
}

const std::vector<MockSignalDef>& mockCatalog() {
    static const std::vector<MockSignalDef> kCatalog = {
        {"Mock.sine_1hz",    "LREAL", DataType::Float64, 0x01,  1000, &genSineLREAL},
        {"Mock.cosine_10hz", "LREAL", DataType::Float64, 0x02,  1000, &genCosineLREAL},
        {"Mock.sawtooth",    "REAL",  DataType::Float32, 0x03,  5000, &genSawtoothREAL},
        {"Mock.counter",     "DINT",  DataType::Int32,   0x04, 10000, &genCounterDINT},
        {"Mock.toggle",      "BOOL",  DataType::Bool,    0x05, 10000, &genToggleBOOL},
        {"Mock.noisy_sine",  "LREAL", DataType::Float64, 0x06,  1000, &genNoisySineLREAL},
    };
    return kCatalog;
}

const MockSignalDef* findByOffset(std::uint32_t indexOffset) {
    for (const auto& s : mockCatalog()) {
        if (s.indexOffset == indexOffset) return &s;
    }
    return nullptr;
}

}  // namespace

struct MockAdsClient::Impl {
    std::atomic<bool> connected{false};

    struct Subscription {
        AdsNotificationHandler handler;
        const MockSignalDef* def;
        std::uint32_t cycleTimeUs;
        std::atomic<bool> stop{false};
        std::thread worker;
    };

    std::mutex mtx;
    std::unordered_map<AdsNotificationHandle, std::unique_ptr<Subscription>> subs;
    std::atomic<AdsNotificationHandle> nextHandle{1};

    void runWorker(Subscription* sub) {
        const std::size_t bytesPerSample = sizeOf(sub->def->dataType);
        std::vector<std::byte> buf(bytesPerSample);
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t i = 0;
        const auto period = std::chrono::microseconds(sub->cycleTimeUs);
        auto next = start;
        while (!sub->stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_until(next);
            const auto now = std::chrono::steady_clock::now();
            const double t = std::chrono::duration<double>(now - start).count();

            sub->def->generate(buf.data(), i, t);

            // Wall-clock nanoseconds so timestamps match nowNs() used by the
            // rest of the app (HDF5, LivePreviewPlot window). steady_clock
            // remains only for the sleep_until pacing.
            const auto wallNs = nowNs();
            AdsSample s;
            s.plcTimestampNs  = wallNs;
            s.hostTimestampNs = wallNs;
            s.data = std::span<const std::byte>(buf.data(), bytesPerSample);
            sub->handler(s);

            ++i;
            next += period;
            // If we've fallen behind by more than one period, skip ahead so we
            // don't burn CPU trying to catch up forever.
            if (next < now - period) next = now + period;
        }
    }
};

MockAdsClient::MockAdsClient() : impl_(std::make_unique<Impl>()) {}
MockAdsClient::~MockAdsClient() { disconnect(); }

bool MockAdsClient::connect(const AdsRoute& /*route*/, QString* /*errorOut*/) {
    impl_->connected = true;
    spdlog::info("MockAdsClient: connected");
    return true;
}

void MockAdsClient::disconnect() {
    if (!impl_->connected.exchange(false)) return;
    std::lock_guard lk(impl_->mtx);
    for (auto& [_, sub] : impl_->subs) {
        sub->stop.store(true, std::memory_order_release);
        if (sub->worker.joinable()) sub->worker.join();
    }
    impl_->subs.clear();
}

bool MockAdsClient::isConnected() const { return impl_->connected.load(); }

std::vector<AdsSymbol> MockAdsClient::listSymbols(QString* /*errorOut*/) {
    std::vector<AdsSymbol> out;
    out.reserve(mockCatalog().size());
    for (const auto& s : mockCatalog()) {
        AdsSymbol sym;
        sym.name        = s.name;
        sym.typeName    = s.typeName;
        sym.dataType    = s.dataType;
        sym.indexGroup  = kMockGroup;
        sym.indexOffset = s.indexOffset;
        sym.size        = static_cast<std::uint32_t>(sizeOf(s.dataType));
        sym.arrayLen    = 1;
        sym.comment     = QString("synthetic, %1 µs cycle").arg(s.taskCycleUs);
        out.push_back(std::move(sym));
    }
    // A structure, listed but opaque — exactly how a real PLC reports one.
    // Its members appear nowhere here; they resolve only by name.
    AdsSymbol st;
    st.name        = "Mock.stAxis";
    st.typeName    = "ST_Axis";
    st.adsDataType = 65;          // ADST_BIGTYPE
    st.unsupported = true;
    st.indexGroup  = kMockGroup;
    st.indexOffset = 0x40;
    st.size        = 0x18;
    st.comment     = "synthetic structure — resolve members by name";
    out.push_back(std::move(st));
    return out;
}

// PLC spelling of a scalar type, for the synthetic members below.
const char* plcTypeNameFor(DataType t) {
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
    return "LREAL";
}

// Members of the synthetic structure below. They are deliberately NOT in
// mockCatalog(), mirroring a real PLC: the symbol upload lists one entry per
// declared variable, so a struct is opaque and its members are reachable only
// by name.
struct MockMember { const char* name; DataType type; std::uint32_t offset; };
const std::vector<MockMember>& mockStructMembers() {
    static const std::vector<MockMember> kMembers = {
        {"Mock.stAxis.fActPos",  DataType::Float64, 0x40},
        {"Mock.stAxis.fActVelo", DataType::Float64, 0x48},
        {"Mock.stAxis.nState",   DataType::Int32,   0x50},
        {"Mock.stAxis.bEnabled", DataType::Bool,    0x54},
    };
    return kMembers;
}

std::optional<AdsSymbol> MockAdsClient::resolveSymbol(const QString& name,
                                                      QString* errorOut) {
    const QString n = name.trimmed();
    if (!isConnected()) {
        if (errorOut) *errorOut = "Not connected.";
        return std::nullopt;
    }
    // A member of the synthetic struct.
    for (const auto& m : mockStructMembers()) {
        if (n != QLatin1String(m.name)) continue;
        AdsSymbol s;
        s.name        = n;
        s.dataType    = m.type;
        s.typeName    = plcTypeNameFor(m.type);
        s.indexGroup  = kMockGroup;
        s.indexOffset = m.offset;
        s.size        = static_cast<std::uint32_t>(sizeOf(m.type));
        s.arrayLen    = 1;
        s.comment     = "synthetic struct member";
        return s;
    }
    // A plain catalogue symbol.
    for (const auto& c : mockCatalog()) {
        if (n != c.name) continue;
        AdsSymbol s;
        s.name        = c.name;
        s.typeName    = c.typeName;
        s.dataType    = c.dataType;
        s.indexGroup  = kMockGroup;
        s.indexOffset = c.indexOffset;
        s.size        = static_cast<std::uint32_t>(sizeOf(c.dataType));
        s.arrayLen    = 1;
        return s;
    }
    // The struct itself: listed, but not one number.
    if (n == QLatin1String("Mock.stAxis")) {
        if (errorOut)
            *errorOut = "'Mock.stAxis' is a ST_Axis, which has no single "
                        "numeric value. Name one of its members instead, "
                        "e.g. Mock.stAxis.fActPos.";
        return std::nullopt;
    }
    if (errorOut)
        *errorOut = QString("The PLC doesn't know a symbol called '%1'.").arg(n);
    return std::nullopt;
}

std::vector<AdsTaskInfo> MockAdsClient::listTasks(QString*) {
    AdsTaskInfo fast {0, "MockFast", 1000,  20, false};
    AdsTaskInfo mid  {1, "MockMid",  5000,  21, false};
    AdsTaskInfo slow {2, "MockSlow", 10000, 22, false};
    return {fast, mid, slow};
}

std::uint32_t MockAdsClient::taskCycleForSymbol(const AdsSymbol& symbol) {
    if (auto* def = findByOffset(symbol.indexOffset)) return def->taskCycleUs;
    return 0;
}

AdsNotificationHandle MockAdsClient::addNotification(
    const AdsNotificationSpec& spec,
    AdsNotificationHandler handler,
    QString* errorOut) {

    const auto* def = findByOffset(spec.indexOffset);
    if (!def) {
        if (errorOut) *errorOut = "Mock symbol not found";
        return 0;
    }

    auto sub = std::make_unique<Impl::Subscription>();
    sub->handler = std::move(handler);
    sub->def = def;
    sub->cycleTimeUs = spec.cycleTimeUs != 0 ? spec.cycleTimeUs : def->taskCycleUs;

    auto* raw = sub.get();
    sub->worker = std::thread([this, raw]{ impl_->runWorker(raw); });

    const auto handle = impl_->nextHandle.fetch_add(1);
    std::lock_guard lk(impl_->mtx);
    impl_->subs.emplace(handle, std::move(sub));
    return handle;
}

void MockAdsClient::removeNotification(AdsNotificationHandle handle) {
    std::unique_ptr<Impl::Subscription> sub;
    {
        std::lock_guard lk(impl_->mtx);
        auto it = impl_->subs.find(handle);
        if (it == impl_->subs.end()) return;
        sub = std::move(it->second);
        impl_->subs.erase(it);
    }
    sub->stop.store(true, std::memory_order_release);
    if (sub->worker.joinable()) sub->worker.join();
}

bool MockAdsClient::read(std::uint32_t /*indexGroup*/,
                         std::uint32_t /*indexOffset*/,
                         std::span<std::byte> /*out*/,
                         QString* errorOut) {
    if (errorOut) *errorOut = "MockAdsClient::read not implemented";
    return false;
}

std::unique_ptr<scope::core::IAdsClient> makeMockAdsClient() {
    return std::make_unique<MockAdsClient>();
}

}  // namespace scope::ads
