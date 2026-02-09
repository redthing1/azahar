// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "core/coverage.h"

namespace Core::Coverage {

namespace {
constexpr u32 kEmptyAddress = 0xFFFFFFFF;
constexpr std::size_t kCoverageTableBits = 20;
constexpr std::size_t kCoverageTableSize = std::size_t{1} << kCoverageTableBits;
constexpr std::size_t kCoverageTableMask = kCoverageTableSize - 1;
constexpr std::size_t kMaxProbes = 64;

struct Slot {
    std::atomic<u32> address{kEmptyAddress};
    std::atomic<u64> hits{0};
};

std::atomic<u8> g_collecting = 0;
std::atomic<bool> g_jit_instrumentation_enabled = false;
std::atomic<u32> g_active_recorders = 0;
std::array<Slot, kCoverageTableSize> g_slots{};
std::mutex g_overflow_mutex;
std::unordered_map<u32, u64> g_overflow_hits;
std::mutex g_jit_counters_mutex;
std::unordered_map<u64, std::unique_ptr<u64>> g_jit_counters;

[[nodiscard]] std::size_t HashAddress(u32 address) {
    constexpr u64 kHashMultiplier = 11400714819323198485ull;
    return (static_cast<u64>(address) * kHashMultiplier) >> (64 - kCoverageTableBits);
}

void WaitForActiveRecorders() {
    while (g_active_recorders.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
}

void ResetJitCounters() {
    std::scoped_lock lock{g_jit_counters_mutex};
    for (auto& [key, hits_ptr] : g_jit_counters) {
        std::atomic_ref<u64>(*hits_ptr).store(0, std::memory_order_relaxed);
    }
}

void ClearCoverageTables() {
    for (auto& slot : g_slots) {
        slot.hits.store(0, std::memory_order_relaxed);
        slot.address.store(kEmptyAddress, std::memory_order_relaxed);
    }
    std::scoped_lock lock{g_overflow_mutex};
    g_overflow_hits.clear();
    ResetJitCounters();
}

class ActiveRecorderGuard {
public:
    ActiveRecorderGuard() {
        g_active_recorders.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveRecorderGuard() {
        g_active_recorders.fetch_sub(1, std::memory_order_acq_rel);
    }
};

[[nodiscard]] bool InsertOrIncrementSlot(u32 address) {
    const std::size_t first_index = HashAddress(address);
    for (std::size_t probe = 0; probe < kMaxProbes; ++probe) {
        Slot& slot = g_slots[(first_index + probe) & kCoverageTableMask];
        const u32 existing = slot.address.load(std::memory_order_acquire);

        if (existing == address) {
            slot.hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        if (existing == kEmptyAddress) {
            u32 expected = kEmptyAddress;
            if (slot.address.compare_exchange_strong(expected, address, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                slot.hits.store(1, std::memory_order_release);
                return true;
            }
            if (expected == address) {
                slot.hits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
    }

    return false;
}
} // namespace

void StartCollection() {
    g_collecting.store(0, std::memory_order_release);
    WaitForActiveRecorders();
    ClearCoverageTables();
    g_collecting.store(1, std::memory_order_release);
}

std::unordered_map<u32, u64> StopCollection() {
    g_collecting.store(0, std::memory_order_release);
    WaitForActiveRecorders();

    std::unordered_map<u32, u64> snapshot;
    snapshot.reserve(131072);

    for (auto& slot : g_slots) {
        const u32 address = slot.address.load(std::memory_order_acquire);
        if (address == kEmptyAddress) {
            continue;
        }
        const u64 hits = slot.hits.load(std::memory_order_acquire);
        if (hits != 0) {
            snapshot.emplace(address, hits);
        }
        slot.hits.store(0, std::memory_order_relaxed);
        slot.address.store(kEmptyAddress, std::memory_order_relaxed);
    }

    {
        std::scoped_lock lock{g_overflow_mutex};
        for (const auto& [address, hits] : g_overflow_hits) {
            snapshot[address] += hits;
        }
        g_overflow_hits.clear();
    }

    {
        std::scoped_lock lock{g_jit_counters_mutex};
        for (const auto& [key, hits_ptr] : g_jit_counters) {
            const u32 address = static_cast<u32>(key & 0xFFFFFFFFu);
            snapshot[address] += std::atomic_ref<u64>(*hits_ptr).load(std::memory_order_relaxed);
        }
    }

    return snapshot;
}

void Clear() {
    g_collecting.store(0, std::memory_order_release);
    WaitForActiveRecorders();
    ClearCoverageTables();
}

void ReleaseStorage() {
    g_collecting.store(0, std::memory_order_release);
    WaitForActiveRecorders();
    ClearCoverageTables();
    std::scoped_lock lock{g_jit_counters_mutex};
    g_jit_counters.clear();
}

void RecordBlock(u32 address) {
    if (!g_collecting.load(std::memory_order_acquire)) {
        return;
    }

    ActiveRecorderGuard active_guard;
    if (!g_collecting.load(std::memory_order_relaxed)) {
        return;
    }

    if (address != kEmptyAddress && InsertOrIncrementSlot(address)) {
        return;
    }

    std::scoped_lock lock{g_overflow_mutex};
    ++g_overflow_hits[address];
}

u64* GetJitCounterPointer(u32 address, u32 core_id) {
    if (!g_collecting.load(std::memory_order_acquire) ||
        !g_jit_instrumentation_enabled.load(std::memory_order_acquire)) {
        return nullptr;
    }

    const u64 key = (static_cast<u64>(core_id) << 32) | address;
    std::scoped_lock lock{g_jit_counters_mutex};
    auto [it, inserted] = g_jit_counters.try_emplace(key);
    if (inserted) {
        it->second = std::make_unique<u64>(0);
    }
    return it->second.get();
}

bool IsCollecting() {
    return g_collecting.load(std::memory_order_acquire) != 0;
}

void SetJitInstrumentationEnabled(bool enabled) {
    g_jit_instrumentation_enabled.store(enabled, std::memory_order_release);
}

bool IsJitInstrumentationEnabled() {
    return g_jit_instrumentation_enabled.load(std::memory_order_acquire);
}

const u8* GetJitCollectingFlagPointer() {
    return reinterpret_cast<const u8*>(&g_collecting);
}

} // namespace Core::Coverage
