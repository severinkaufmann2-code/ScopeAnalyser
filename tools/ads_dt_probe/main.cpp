// ads_dt_probe — capture a PLC's raw symbol and data-type tables.
//
// Why this exists: a structure is one entry in the symbol upload, so its
// member names and byte offsets appear in no list. They live in the ADS
// data-type table (ADSIGRP_SYM_DT_UPLOAD, 0xF00E), and neither that table's
// wire layout nor the ADST_* codes are described by the Beckhoff headers this
// project vendors. Writing a parser for it from documentation alone would mean
// testing my reading of the format against itself.
//
// So: dump the real bytes once, commit them as a test fixture, and develop the
// parser offline against ground truth.
//
// It reuses RouterAdsClient — the same client the Recorder connects with — so
// nothing about the protocol is re-implemented here.
//
//   ads_dt_probe <host> <target-netid> [ads-port] [out-dir]
//   ads_dt_probe 192.168.1.50 5.123.45.67.1.1 851 .
//
// Writes ads_uploadinfo.bin (24 B), ads_symbols.bin, ads_datatypes.bin and
// prints a summary. Reads only — it never writes to the PLC.

#include "scope/core/IAdsClient.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kSymUpload       = 0xF00B;
constexpr std::uint32_t kSymDtUpload     = 0xF00E;
constexpr std::uint32_t kSymUploadInfo2  = 0xF00F;

std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

bool dump(const QString& dir, const char* file, const std::vector<std::byte>& d) {
    QFile f(QDir(dir).filePath(file));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "  ! couldn't write %s: %s\n", file,
                     qPrintable(f.errorString()));
        return false;
    }
    f.write(reinterpret_cast<const char*>(d.data()),
            static_cast<qint64>(d.size()));
    std::printf("  wrote %-20s %zu bytes\n", file, d.size());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 3) {
        std::printf(
            "ads_dt_probe — dump a PLC's symbol and data-type tables\n\n"
            "  ads_dt_probe <host> <target-netid> [ads-port] [out-dir]\n\n"
            "Example:\n"
            "  ads_dt_probe 127.0.0.1 5.123.45.67.1.1 851 .\n\n"
            "Use the same host / AMS NetId / port the Recorder connects with.\n"
            "Reads only; nothing is written to the PLC.\n");
        return 2;
    }

    scope::core::AdsRoute route;
    route.host  = args.at(1);
    route.netId = args.at(2);
    route.port  = args.size() > 3 ? static_cast<std::uint16_t>(args.at(3).toUShort())
                                  : std::uint16_t{851};
    const QString outDir = args.size() > 4 ? args.at(4) : QStringLiteral(".");

    std::printf("connecting to %s (NetId %s, port %u) …\n",
                qPrintable(route.host), qPrintable(route.netId), route.port);

    auto client = scope::core::makeDefaultAdsClient();
    QString err;
    if (!client->connect(route, &err)) {
        std::fprintf(stderr, "connect failed: %s\n", qPrintable(err));
        return 1;
    }
    std::printf("connected.\n");

    // 1. Upload info: how big the two tables are.
    std::vector<std::byte> info(24);
    if (!client->read(kSymUploadInfo2, 0, info, &err)) {
        std::fprintf(stderr, "SYM_UPLOADINFO2 (0xF00F) failed: %s\n", qPrintable(err));
        return 1;
    }
    const auto* u = reinterpret_cast<const unsigned char*>(info.data());
    const std::uint32_t nSymbols       = le32(u + 0);
    const std::uint32_t nSymSize       = le32(u + 4);
    const std::uint32_t nDatatypes     = le32(u + 8);
    const std::uint32_t nDatatypeSize  = le32(u + 12);
    std::printf("  symbols:   %u entries, %u bytes\n", nSymbols, nSymSize);
    std::printf("  datatypes: %u entries, %u bytes\n", nDatatypes, nDatatypeSize);
    dump(outDir, "ads_uploadinfo.bin", info);

    // 2. The symbol table — so the fixture is self-contained and the parser
    //    can be checked against names we can actually see in TwinCAT.
    if (nSymSize > 0) {
        std::vector<std::byte> syms(nSymSize);
        if (client->read(kSymUpload, 0, syms, &err))
            dump(outDir, "ads_symbols.bin", syms);
        else
            std::fprintf(stderr, "  ! SYM_UPLOAD (0xF00B) failed: %s\n", qPrintable(err));
    }

    // 3. The data-type table — the whole point of this exercise.
    if (nDatatypeSize == 0) {
        std::fprintf(stderr,
            "  ! the PLC reports an empty data-type table. Nothing to capture;\n"
            "    structure members can't be enumerated from this target.\n");
        return 1;
    }
    std::vector<std::byte> types(nDatatypeSize);
    if (!client->read(kSymDtUpload, 0, types, &err)) {
        std::fprintf(stderr, "SYM_DT_UPLOAD (0xF00E) failed: %s\n", qPrintable(err));
        return 1;
    }
    dump(outDir, "ads_datatypes.bin", types);

    std::printf("\ndone. Send the three .bin files back — they become the test\n"
                "fixture the type-table parser is developed against.\n");
    return 0;
}
