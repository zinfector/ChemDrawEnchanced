#pragma once
#include "runtime.hpp"

namespace cd {
void installChemistryWorker();
// UI-only preflight. False keeps the native queue intact until a result is ready.
bool preparePlacementChemistry(Obj page);
bool chemistryWorkerPending(Obj page) noexcept;
void forgetChemistryWorkerPage(Obj page) noexcept;
void trimChemistryWorker() noexcept;
bool chemistryWorkerAvailable() noexcept;
struct ChemistryWorkerStats {
    uint64_t submitted{},hits{},failed{},cancelled{},ticks{},maximum{},preflights{},groups{},misses{};
    std::array<uint64_t,6> rejected{};
    std::array<uint64_t,7> metadataRejected{};
    uint64_t nativeCalls{},nativeTicks{},nativeMaximum{},nativeMaximumCaller{};
    unsigned workers{};
    uint64_t fastPaths{},componentReuse{};
    std::array<uint64_t,5> missesByReason{};
};
ChemistryWorkerStats chemistryWorkerStats() noexcept;
}
