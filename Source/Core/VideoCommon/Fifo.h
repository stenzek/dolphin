// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include "Common/CommonTypes.h"

class PointerWrap;

namespace Fifo
{
// This constant controls the size of the video buffer, which is where the data from the FIFO
// located in guest memory is copied to. GPU commands are executed from this buffer.
constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;

// This constant controls the execution threshold for the GPU. In single core mode, this defines
// the size of the batches which are copied out of guest memory to the video buffer. In dual core
// more, batches will be copied to the video buffer for execution on the GPU thread every time
// the FIFO reaches this size.
constexpr u32 FIFO_EXECUTE_THRESHOLD_SIZE = 512;

// This constant controls the interval at which the GPU will be executed in single-core mode, or
// synchronized with the GPU thread in dual-core mode.
constexpr int GPU_TIME_SLOT_SIZE = 1050;

extern u8* video_buffer;
extern u8* video_buffer_read_ptr;           // owned by GPU thread
extern u8* video_buffer_write_ptr;          // owned by CPU thread
extern std::atomic<u32> video_buffer_size;  // modified by both threads

// In deterministic dual core mode, the pp size is the size available to the GPU thread.
extern u8* video_buffer_pp_read_ptr;
extern std::atomic<u32> video_buffer_pp_size;

// The lock synchronizes access from the CPU thread when the buffer wraps around. This is because
// the data must be contiguous for the vertex loader, so we can't use a circular buffer.
extern std::mutex video_buffer_mutex;

void Init();
void Shutdown();
void Prepare();  // Must be called from the CPU thread.
void DoState(PointerWrap& f);
void PauseAndLock(bool doLock, bool unpauseOnUnlock);
void UpdateWantDeterminism(bool want);
bool UseDeterministicGPUThread();

void PushFifoAuxBuffer(const void* ptr, size_t size);
void* PopFifoAuxBuffer(size_t size);

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
void SyncGPU(SyncGPUReason reason);

// Runs any pending GPU cycles, to execute any commands inbetween JIT blocks/sync events.
// If is_write is set, the GPU will be allowed to "run ahead". This is so that we do not lose
// commands when in multi-FIFO (non-immediate) mode, when switching buffers.
void SyncGPUForRegisterAccess(bool is_write);

void WakeGpu();
void RunGpu(bool allow_run_ahead);
void FlushGpu();
void GpuMaySleep();
void RunGpuLoop();
void ExitGpuLoop();
void EmulatorState(bool running);

// Returns true if the GPU is in the process of executing a command.
bool IsGPUIdle();

// Clears the video buffer.
void ResetVideoBuffer();

// Wakes the GPU if needed.
void UpdateGPUSuspendState();

}  // namespace Fifo
