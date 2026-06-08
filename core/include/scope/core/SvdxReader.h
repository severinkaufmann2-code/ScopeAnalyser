#pragma once

#include "Signal.h"

#include <QString>

#include <filesystem>
#include <memory>
#include <vector>

namespace scope::core {

// Reader for TwinCAT Scope View ".svdx" exports (binary). The container is:
//
//   [u64 xmlLen][u64 xmlOffset]
//   [u32 blockCount @0x10]
//   [blockCount * (u64 offset, u64 length, u32 index)]   // block index
//   [ ... data blocks ... ]
//   [ XML footer: <ScopeProject ...> with per-channel metadata ]
//
// Each data block holds one channel's samples as a sequence of sub-segments:
//
//   ["01.00.00.40"][u64 startFILETIME][u64 dur][u64 segStartFT][u64 segEndFT]
//   ... then repeated:
//     [u64 subsegFILETIME][u32 sampleCount][sampleCount * (u32 tsTicks, value)]
//
// `value` is `sizeOf(dataType)` bytes; `tsTicks` is the offset from the
// sub-segment's FILETIME in 100 ns units. Absolute time = subsegFILETIME +
// tsTicks (converted to ns-since-Unix-epoch). Verified byte-exact against a
// TwinCAT-exported CSV of the same recording.
//
// Read-only: returns one Signal per recorded channel. On failure returns an
// empty vector and sets *errorOut.
std::vector<std::shared_ptr<Signal>> readSvdx(const std::filesystem::path& path,
                                              QString* errorOut = nullptr);

}  // namespace scope::core
