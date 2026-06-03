#include "scope/recorder/OversampledChannel.h"

namespace scope::recorder {

// Phase-2 stub. Symbols are present so RecordingSession can compile with the
// full mode set, but the actual fan-out + per-sample timestamp synthesis
// will land alongside the live preview plot polish in Phase 2.

bool OversampledChannel::arm(scope::core::IAdsClient& /*client*/, QString* errorOut) {
    if (errorOut) *errorOut = "OversampledChannel: not implemented in Phase 1";
    return false;
}

void OversampledChannel::disarm(scope::core::IAdsClient& /*client*/) {}

std::size_t OversampledChannel::drainTo(scope::core::Signal& /*signal*/, std::size_t /*maxBatch*/) {
    return 0;
}

}  // namespace scope::recorder
