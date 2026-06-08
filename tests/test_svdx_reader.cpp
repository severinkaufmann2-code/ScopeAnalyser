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
