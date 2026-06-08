#include "scope/ads/RouterAdsClient.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kBadSock = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kBadSock = -1;
#endif

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace scope::ads {

using namespace scope::core;

namespace {

// AMS command ids and state flags.
constexpr std::uint16_t kRead = 2, kReadState = 4, kAddNote = 6, kDelNote = 7,
                        kDeviceNote = 8;
constexpr std::uint16_t kStateReq = 0x0004;  // request + ADS command
constexpr std::uint16_t kTcpData = 0x0000, kTcpPortConnect = 0x1000;

constexpr std::uint32_t kAdsigrpSymUploadInfo2 = 0xF00F;
constexpr std::uint32_t kAdsigrpSymUpload = 0xF00B;

// AMS notification timestamps are Windows FILETIMEs (100 ns ticks since 1601).
// Convert to ns since the Unix epoch. NB: scaling the raw FILETIME by 100
// overflows int64 (a 2026 FILETIME is ~1.34e17, ×100 ≈ 1.34e19 > INT64_MAX),
// so subtract the epoch offset first.
constexpr std::uint64_t kFiletimeUnixDiff = 116444736000000000ULL;

#pragma pack(push, 1)
struct SymbolEntryHeader {
    std::uint32_t entryLength;
    std::uint32_t indexGroup;
    std::uint32_t indexOffset;
    std::uint32_t size;
    std::uint32_t dataType;
    std::uint32_t flags;       // 4 bytes (not 2) — TwinCAT AdsSymbolEntry layout
    std::uint16_t nameLen;
    std::uint16_t typeLen;
    std::uint16_t commentLen;
};
#pragma pack(pop)

DataType mapAdsDataType(std::uint32_t adst) {
    switch (adst) {
        case 33: return DataType::Bool;
        case 16: return DataType::Int8;
        case 17: return DataType::Uint8;
        case 2:  return DataType::Int16;
        case 18: return DataType::Uint16;
        case 3:  return DataType::Int32;
        case 19: return DataType::Uint32;
        case 20: return DataType::Int64;
        case 21: return DataType::Uint64;
        case 4:  return DataType::Float32;
        case 5:  return DataType::Float64;
        default: return DataType::Float64;
    }
}

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff);
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xff);
}
std::uint16_t g16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
std::uint32_t g32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t g64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(g32(p)) | (static_cast<std::uint64_t>(g32(p + 4)) << 32);
}

}  // namespace

struct RouterAdsClient::Impl {
    socket_t sock{kBadSock};
    std::array<std::uint8_t, 6> srcNet{};
    std::uint16_t srcPort{0};
    std::array<std::uint8_t, 6> tgtNet{};
    std::uint16_t tgtPort{851};
    std::atomic<bool> connected{false};

    std::thread reader;
    std::atomic<std::uint32_t> invokeCtr{1};

    struct Pending {
        std::mutex m;
        std::condition_variable cv;
        bool done{false};
        std::uint32_t amsError{0};
        std::vector<std::uint8_t> data;  // bytes after the 32-byte AMS header
    };
    std::mutex pendMtx;
    std::unordered_map<std::uint32_t, Pending*> pending;

    std::mutex subMtx;
    std::unordered_map<std::uint32_t, AdsNotificationHandler> byNotif;     // PLC handle -> cb
    std::unordered_map<AdsNotificationHandle, std::uint32_t> handleToNotif;
    std::atomic<AdsNotificationHandle> nextHandle{1};

    ~Impl() { close(); }

    void close() {
        connected = false;
        if (sock != kBadSock) {
#ifdef _WIN32
            shutdown(sock, SD_BOTH); closesocket(sock);
#else
            ::shutdown(sock, SHUT_RDWR); ::close(sock);
#endif
            sock = kBadSock;
        }
        if (reader.joinable()) reader.join();
    }

    bool sendAll(const void* p, std::size_t n) {
        const char* c = static_cast<const char*>(p);
        while (n) {
#ifdef _WIN32
            int k = ::send(sock, c, static_cast<int>(n), 0);
#else
            ssize_t k = ::send(sock, c, n, 0);
#endif
            if (k <= 0) return false;
            c += k; n -= static_cast<std::size_t>(k);
        }
        return true;
    }
    bool recvAll(void* p, std::size_t n) {
        char* c = static_cast<char*>(p);
        while (n) {
#ifdef _WIN32
            int k = ::recv(sock, c, static_cast<int>(n), 0);
#else
            ssize_t k = ::recv(sock, c, n, 0);
#endif
            if (k <= 0) return false;
            c += k; n -= static_cast<std::size_t>(k);
        }
        return true;
    }

    bool sendFrame(std::uint16_t cmd, const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> h;
        put16(h, cmd);
        put32(h, static_cast<std::uint32_t>(payload.size()));
        return sendAll(h.data(), h.size()) &&
               (payload.empty() || sendAll(payload.data(), payload.size()));
    }

    // Build + send an AMS request, returning its invokeId.
    std::uint32_t sendAms(std::uint16_t cmdId, const std::vector<std::uint8_t>& data) {
        const std::uint32_t inv = invokeCtr.fetch_add(1);
        std::vector<std::uint8_t> h;
        h.insert(h.end(), tgtNet.begin(), tgtNet.end()); put16(h, tgtPort);
        h.insert(h.end(), srcNet.begin(), srcNet.end()); put16(h, srcPort);
        put16(h, cmdId); put16(h, kStateReq);
        put32(h, static_cast<std::uint32_t>(data.size())); put32(h, 0); put32(h, inv);
        h.insert(h.end(), data.begin(), data.end());
        sendFrame(kTcpData, h);
        return inv;
    }

    // Synchronous request: returns AMS error (0 = ok) and the data after the
    // 32-byte AMS header (most ADS replies begin with a u32 ADS result).
    std::uint32_t request(std::uint16_t cmdId, const std::vector<std::uint8_t>& data,
                          std::vector<std::uint8_t>& out, int timeoutMs = 5000) {
        Pending p;
        std::uint32_t inv;
        {
            std::lock_guard lk(pendMtx);
            inv = invokeCtr.load();  // reserved by sendAms below
        }
        // Register before sending so the reader can find it.
        {
            std::lock_guard lk(pendMtx);
            inv = sendAms(cmdId, data);
            pending[inv] = &p;
        }
        std::unique_lock lk(p.m);
        if (!p.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [&] { return p.done; })) {
            std::lock_guard g(pendMtx); pending.erase(inv);
            return 1861;  // ADSERR_CLIENT_SYNCTIMEOUT
        }
        out = std::move(p.data);
        return p.amsError;
    }

    void readerLoop() {
        while (connected) {
            std::uint8_t hdr[6];
            if (!recvAll(hdr, 6)) break;
            const std::uint32_t len = g32(hdr + 2);
            std::vector<std::uint8_t> payload(len);
            if (len && !recvAll(payload.data(), len)) break;
            if (payload.size() < 32) continue;

            const std::uint16_t cmdId = g16(payload.data() + 16);
            const std::uint32_t amsErr = g32(payload.data() + 24);
            const std::uint32_t inv = g32(payload.data() + 28);
            std::vector<std::uint8_t> data(payload.begin() + 32, payload.end());

            if (cmdId == kDeviceNote) { dispatchNotifications(data); continue; }

            std::lock_guard lk(pendMtx);
            auto it = pending.find(inv);
            if (it != pending.end()) {
                Pending* p = it->second; pending.erase(it);
                std::lock_guard g(p->m);
                p->amsError = amsErr; p->data = std::move(data); p->done = true;
                p->cv.notify_one();
            }
        }
        connected = false;
        // Wake any waiters so they time out / unblock.
        std::lock_guard lk(pendMtx);
        for (auto& [inv, p] : pending) {
            std::lock_guard g(p->m); p->amsError = 1861; p->done = true; p->cv.notify_one();
        }
        pending.clear();
    }

    // Parse an AdsNotificationStream and fan out samples to handlers.
    void dispatchNotifications(const std::vector<std::uint8_t>& d) {
        if (d.size() < 8) return;
        std::size_t o = 4;                       // skip stream length
        std::uint32_t stamps = g32(d.data() + o); o += 4;
        for (std::uint32_t s = 0; s < stamps && o + 12 <= d.size(); ++s) {
            const std::uint64_t ft = g64(d.data() + o); o += 8;
            const std::uint32_t samples = g32(d.data() + o); o += 4;
            for (std::uint32_t k = 0; k < samples && o + 8 <= d.size(); ++k) {
                const std::uint32_t notif = g32(d.data() + o); o += 4;
                const std::uint32_t size = g32(d.data() + o); o += 4;
                if (o + size > d.size()) return;
                AdsNotificationHandler cb;
                {
                    std::lock_guard lk(subMtx);
                    auto it = byNotif.find(notif);
                    if (it != byNotif.end()) cb = it->second;
                }
                if (cb) {
                    AdsSample smp;
                    smp.plcTimestampNs = ft >= kFiletimeUnixDiff
                        ? static_cast<TimestampNs>((ft - kFiletimeUnixDiff) * 100ULL)
                        : 0;
                    smp.hostTimestampNs = nowNs();
                    smp.data = std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(d.data() + o), size);
                    cb(smp);
                }
                o += size;
            }
        }
    }
};

RouterAdsClient::RouterAdsClient() : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
}
RouterAdsClient::~RouterAdsClient() { disconnect(); }

bool RouterAdsClient::connect(const AdsRoute& route, QString* errorOut) {
    disconnect();
    auto fail = [&](const QString& m) {
        if (errorOut) *errorOut = m;
        impl_->close();
        return false;
    };

    // Parse target NetId "a.b.c.d.e.f". Empty → use the router's own NetId
    // (resolved from the port-connect reply) so a local PLC needs no NetId.
    const bool autoLocalNetId = route.netId.trimmed().isEmpty();
    if (!autoLocalNetId) {
        int n[6] = {0};
        if (std::sscanf(route.netId.toStdString().c_str(), "%d.%d.%d.%d.%d.%d",
                        &n[0], &n[1], &n[2], &n[3], &n[4], &n[5]) != 6)
            return fail("Invalid AMS NetId (expected a.b.c.d.e.f).");
        for (int i = 0; i < 6; ++i) impl_->tgtNet[i] = static_cast<std::uint8_t>(n[i]);
    }
    impl_->tgtPort = route.port;

    const QString host = route.host.trimmed().isEmpty()
        ? (autoLocalNetId ? QString("127.0.0.1") : route.netId.section('.', 0, 3))
        : route.host.trimmed();

    // Resolve + connect to the router (TCP 48898).
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.toStdString().c_str(), "48898", &hints, &res) != 0 || !res)
        return fail(QString("Couldn't resolve host '%1'.").arg(host));
    impl_->sock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    bool ok = impl_->sock != kBadSock &&
              ::connect(impl_->sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
    ::freeaddrinfo(res);
    if (!ok) return fail(QString("Couldn't connect to AMS router at %1:48898.").arg(host));
    {
        int one = 1;
        ::setsockopt(impl_->sock, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));
    }

    // Register a port with the router → it assigns our source AMS address.
    {
        std::vector<std::uint8_t> p; put16(p, 0);
        if (!impl_->sendFrame(kTcpPortConnect, p))
            return fail("AMS port-connect send failed.");
        std::uint8_t h[6];
        if (!impl_->recvAll(h, 6)) return fail("Router closed the connection.");
        const std::uint32_t len = g32(h + 2);
        std::vector<std::uint8_t> r(len);
        if (len && !impl_->recvAll(r.data(), len))
            return fail("Router closed during port-connect.");
        if (r.size() < 8) return fail("Bad port-connect reply.");
        std::memcpy(impl_->srcNet.data(), r.data(), 6);
        impl_->srcPort = g16(r.data() + 6);
    }

    // For a local PLC with no NetId given, target the router's own NetId.
    if (autoLocalNetId) impl_->tgtNet = impl_->srcNet;

    impl_->connected = true;
    impl_->reader = std::thread([this] { impl_->readerLoop(); });

    // Confirm the PLC port answers.
    std::vector<std::uint8_t> out;
    if (impl_->request(kReadState, {}, out) != 0 || out.size() < 4 || g32(out.data()) != 0) {
        return fail(QString("Connected to router but PLC port %1 did not respond "
                            "(is the runtime in Run mode?).").arg(route.port));
    }
    spdlog::info("ADS router-client connected: router={} target={}:{} src={}.{}.{}.{}.{}.{}:{}",
                 host.toStdString(), route.netId.toStdString(), route.port,
                 impl_->srcNet[0], impl_->srcNet[1], impl_->srcNet[2], impl_->srcNet[3],
                 impl_->srcNet[4], impl_->srcNet[5], impl_->srcPort);
    return true;
}

void RouterAdsClient::disconnect() {
    {
        std::lock_guard lk(impl_->subMtx);
        impl_->byNotif.clear();
        impl_->handleToNotif.clear();
    }
    impl_->close();
}

bool RouterAdsClient::isConnected() const { return impl_->connected.load(); }

bool RouterAdsClient::read(std::uint32_t indexGroup, std::uint32_t indexOffset,
                           std::span<std::byte> out, QString* errorOut) {
    if (!impl_->connected) { if (errorOut) *errorOut = "Not connected"; return false; }
    std::vector<std::uint8_t> d;
    put32(d, indexGroup); put32(d, indexOffset); put32(d, static_cast<std::uint32_t>(out.size()));
    std::vector<std::uint8_t> r;
    const std::uint32_t ams = impl_->request(kRead, d, r);
    if (ams != 0 || r.size() < 8 || g32(r.data()) != 0) {
        if (errorOut) *errorOut = QString("ADS read failed (err %1).").arg(ams ? ams : g32(r.data()));
        return false;
    }
    const std::uint32_t n = g32(r.data() + 4);
    if (r.size() < 8 + n || n > out.size()) { if (errorOut) *errorOut = "Short ADS read."; return false; }
    std::memcpy(out.data(), r.data() + 8, n);
    return true;
}

std::vector<AdsSymbol> RouterAdsClient::listSymbols(QString* errorOut) {
    if (!impl_->connected) { if (errorOut) *errorOut = "Not connected"; return {}; }
    // SYM_UPLOADINFO2 returns a 24-byte struct; ask for all of it (some servers
    // reject a short read). We only need the first two u32s.
    struct { std::uint32_t nSymbols, nSymSize; } info{};
    std::byte infoBuf[24];
    if (!read(kAdsigrpSymUploadInfo2, 0, std::span<std::byte>(infoBuf, 24), errorOut)) return {};
    std::memcpy(&info, infoBuf, 8);
    if (info.nSymSize == 0) return {};

    std::vector<std::uint8_t> blob(info.nSymSize);
    {
        std::vector<std::uint8_t> d;
        put32(d, kAdsigrpSymUpload); put32(d, 0); put32(d, info.nSymSize);
        std::vector<std::uint8_t> r;
        const std::uint32_t ams = impl_->request(kRead, d, r);
        if (ams != 0 || r.size() < 8 || g32(r.data()) != 0) {
            if (errorOut) *errorOut = "Symbol upload failed.";
            return {};
        }
        const std::uint32_t n = g32(r.data() + 4);
        blob.assign(r.begin() + 8, r.begin() + 8 + std::min<std::size_t>(n, r.size() - 8));
    }

    std::vector<AdsSymbol> outv;
    const std::uint8_t* p = blob.data();
    const std::uint8_t* end = blob.data() + blob.size();
    while (p + sizeof(SymbolEntryHeader) <= end) {
        SymbolEntryHeader h; std::memcpy(&h, p, sizeof(h));
        if (h.entryLength == 0 || p + h.entryLength > end) break;
        const char* base = reinterpret_cast<const char*>(p + sizeof(SymbolEntryHeader));
        AdsSymbol s;
        s.name = QString::fromUtf8(base, h.nameLen);
        s.typeName = QString::fromUtf8(base + h.nameLen + 1, h.typeLen);
        s.comment = QString::fromUtf8(base + h.nameLen + 1 + h.typeLen + 1, h.commentLen);
        s.indexGroup = h.indexGroup;
        s.indexOffset = h.indexOffset;
        s.size = h.size;
        s.dataType = mapAdsDataType(h.dataType);
        const std::size_t elem = sizeOf(s.dataType);
        s.arrayLen = elem > 0 ? (s.size / static_cast<std::uint32_t>(elem)) : 1;
        outv.push_back(std::move(s));
        p += h.entryLength;
    }
    return outv;
}

std::vector<AdsTaskInfo> RouterAdsClient::listTasks(QString* errorOut) {
    if (errorOut) *errorOut = "listTasks(): not implemented; use taskCycleForSymbol()";
    return {};
}

std::uint32_t RouterAdsClient::taskCycleForSymbol(const AdsSymbol&) { return 0; }

AdsNotificationHandle RouterAdsClient::addNotification(const AdsNotificationSpec& spec,
                                                       AdsNotificationHandler handler,
                                                       QString* errorOut) {
    if (!impl_->connected) { if (errorOut) *errorOut = "Not connected"; return 0; }
    std::vector<std::uint8_t> d;
    put32(d, spec.indexGroup); put32(d, spec.indexOffset); put32(d, spec.length);
    put32(d, spec.maxAgeUs == 0 ? 3u : 4u);   // ADSTRANS_SERVERCYCLE / SERVERONCHA
    put32(d, 0);                              // maxDelay
    put32(d, spec.cycleTimeUs * 10);          // cycle time in 100 ns ticks
    for (int i = 0; i < 16; ++i) d.push_back(0);
    std::vector<std::uint8_t> r;
    const std::uint32_t ams = impl_->request(kAddNote, d, r);
    if (ams != 0 || r.size() < 8 || g32(r.data()) != 0) {
        if (errorOut) *errorOut = QString("addNotification failed (err %1).")
                                      .arg(ams ? ams : g32(r.data()));
        return 0;
    }
    const std::uint32_t notif = g32(r.data() + 4);
    const auto handle = impl_->nextHandle.fetch_add(1);
    std::lock_guard lk(impl_->subMtx);
    impl_->byNotif[notif] = std::move(handler);
    impl_->handleToNotif[handle] = notif;
    return handle;
}

void RouterAdsClient::removeNotification(AdsNotificationHandle handle) {
    std::uint32_t notif = 0;
    {
        std::lock_guard lk(impl_->subMtx);
        auto it = impl_->handleToNotif.find(handle);
        if (it == impl_->handleToNotif.end()) return;
        notif = it->second;
        impl_->handleToNotif.erase(it);
        impl_->byNotif.erase(notif);
    }
    if (impl_->connected) {
        std::vector<std::uint8_t> d; put32(d, notif);
        std::vector<std::uint8_t> r;
        impl_->request(kDelNote, d, r);
    }
}

}  // namespace scope::ads

namespace scope::core {
std::unique_ptr<IAdsClient> makeDefaultAdsClient() {
    return std::make_unique<scope::ads::RouterAdsClient>();
}
}  // namespace scope::core
