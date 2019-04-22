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
void UpdateWantDeterminism(bool want);
bool UseDeterministicGPUThread();

// Used for diagnostics.
enum class SyncGPUReason
{
  Other,
  Wraparound,
  EFBPoke,
  PerfQuery,
  BBox,
  Swap,
  AuxSpace,
};
// In deterministic GPU thread mode this waits for the GPU to be done with pending work.
void SyncGPU(SyncGPUReason reason, bool may_move_read_ptr = true);

// Runs any pending GPU cycles, to execute any commands inbetween JIT blocks/sync events.
// If is_write is set, the GPU will be allowed to "run ahead". This is so that we do not lose
// commands when in multi-FIFO (non-immediate) mode, when switching buffers.
void SyncGPUForRegisterAccess(bool is_write);

void PushFifoAuxBuffer(const void* ptr, size_t size);
void* PopFifoAuxBuffer(size_t size);
u32 GetVideoBufferSize();

void FlushGpu();
void RunGpu(bool allow_runahead);
void GpuMaySleep();
void RunGpuLoop();
void ExitGpuLoop();
void EmulatorState(bool running);
void ResetVideoBuffer();

}  // namespace Fifo
