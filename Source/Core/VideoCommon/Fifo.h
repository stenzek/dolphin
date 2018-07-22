// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include "Common/CommonTypes.h"

class PointerWrap;

namespace Fifo
{
void Init();
void Shutdown();
void Prepare();  // Must be called from the CPU thread.
void DoState(PointerWrap& f);
void PauseAndLock(bool doLock, bool unpauseOnUnlock);
void ReadDataFromFifo(u32 readPtr, size_t len);

void WakeGpu();
void GpuMaySleep();
void RunGpuLoop();
void ExitGpuLoop();
void EmulatorState(bool running);
void ResetVideoBuffer();
void WaitForGpu(bool idle);

}  // namespace Fifo
