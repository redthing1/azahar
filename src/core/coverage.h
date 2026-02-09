// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <unordered_map>
#include "common/common_types.h"

namespace Core::Coverage {

void StartCollection();
[[nodiscard]] std::unordered_map<u32, u64> StopCollection();
void Clear();
void ReleaseStorage();

void RecordBlock(u32 address);
[[nodiscard]] u64* GetJitCounterPointer(u32 address, u32 core_id);
[[nodiscard]] const u8* GetJitCollectingFlagPointer();

[[nodiscard]] bool IsCollecting();

void SetJitInstrumentationEnabled(bool enabled);
[[nodiscard]] bool IsJitInstrumentationEnabled();

} // namespace Core::Coverage
