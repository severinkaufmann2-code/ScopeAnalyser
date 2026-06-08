#include "scope/core/SvdxReader.h"
#include "scope/core/Signal.h"

#include <gtest/gtest.h>

#include <filesystem>

using namespace scope::core;

#ifndef SVDX_TEST_DATA_DIR
#define SVDX_TEST_DATA_DIR "."
#endif

// Decodes a real TwinCAT Scope ".svdx" export and checks it byte-for-byte
// against the values TwinCAT itself wrote to the sibling CSV: a single BIT
// channel "test", toggling 0/1 every 10 ms for 355 samples (0..3540 ms).
TEST(SvdxReader, DecodesTwinCatBitChannel) {
    const auto path = std::filesystem::path(SVDX_TEST_DATA_DIR) / "TestScope.svdx";
    ASSERT_TRUE(std::filesystem::exists(path)) << path.string();

    QString err;
    auto chans = readSvdx(path, &err);
    ASSERT_FALSE(chans.empty()) << err.toStdString();
    ASSERT_EQ(chans.size(), 1u);

    const auto& sig = chans[0];
    EXPECT_EQ(sig->meta().name, "test");
    EXPECT_EQ(sig->meta().dataType, DataType::Bool);
    EXPECT_EQ(sig->sampleCount(), 355u);

    auto view = sig->snapshotForRead();
    ASSERT_EQ(view.count, 355u);

    // First sample at t=0; spacing 10 ms (= 10'000'000 ns); values alternate
    // 0,1,0,1,... exactly as the TwinCAT CSV reports.
    const auto* ts = view.timestamps;
    const auto* vals = reinterpret_cast<const std::uint8_t*>(view.values);
    for (std::size_t i = 0; i < view.count; ++i) {
        EXPECT_EQ(vals[i], i % 2 == 0 ? 0u : 1u) << "sample " << i;
        if (i > 0) {
            EXPECT_EQ(ts[i] - ts[i - 1], 10'000'000)
                << "spacing at sample " << i;
        }
    }
}

// Local-only: verify REAL64 decoding against a full TwinCAT recording. The
// 649 MB file is too large to commit, so this skips unless it's present in
// the working tree. The expected values are the first ActVelo samples from
// the matching TwinCAT CSV export (TestScope2.csv).
TEST(SvdxReader, DecodesReal64ChannelIfBigFilePresent) {
    const std::filesystem::path path =
        std::filesystem::path(SVDX_TEST_DATA_DIR) / ".." / ".." /
        "040626_Test_In10HminSchritteBis150Hmin.svdx";
    if (!std::filesystem::exists(path))
        GTEST_SKIP() << "big sample not present: " << path.string();

    QString err;
    auto chans = readSvdx(path, &err);
    ASSERT_FALSE(chans.empty()) << err.toStdString();

    const std::vector<double> expected{
        -0.014977340885399332, -0.01524553118508775, -0.027307922355041477,
        -0.021174928990280131, -0.0174325739674399,  -0.0038614194296103712};

    bool found = false;
    for (const auto& sig : chans) {
        if (sig->meta().dataType != DataType::Float64) continue;
        const auto v = sig->readAsDouble();
        for (std::size_t i = 0; i + expected.size() <= v.size() && !found; ++i) {
            bool all = true;
            for (std::size_t j = 0; j < expected.size(); ++j)
                if (std::abs(v[i + j] - expected[j]) > 1e-9) { all = false; break; }
            if (all) found = true;
        }
        if (found) break;
    }
    EXPECT_TRUE(found) << "ActVelo REAL64 sample sequence not found in any channel";
}
