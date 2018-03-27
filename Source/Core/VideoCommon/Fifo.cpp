// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/Fifo.h"

#include <atomic>
#include <cstring>

#include "Common/Assert.h"
#include "Common/Atomic.h"
#include "Common/BlockingLoop.h"
#include "Common/ChunkFile.h"
#include "Common/Event.h"
#include "Common/FPURoundMode.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"

#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Fifo
{
static constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;
static constexpr int GPU_TIME_SLOT_SIZE = 1000;

static Common::BlockingLoop s_gpu_mainloop;

static Common::Flag s_emu_running_state;
static CoreTiming::EventType* s_event_sync_gpu;

// STATE_TO_SAVE
static u8* s_video_buffer;
static u8* s_video_buffer_read_ptr;
static u8* s_video_buffer_write_ptr;
// The read_ptr is always owned by the GPU thread.  In normal mode, so is the
// write_ptr, despite it being atomic.  In deterministic GPU thread mode,
// things get a bit more complicated:
// - The seen_ptr is written by the GPU thread, and points to what it's already
// processed as much of as possible - in the case of a partial command which
// caused it to stop, not the same as the read ptr.  It's written by the GPU,
// under the lock, and updating the cond.
// - The write_ptr is written by the CPU thread after it copies data from the
// FIFO.  Maybe someday it will be under the lock.  For now, because RunGpuLoop
// polls, it's just atomic.
// - The pp_read_ptr is the CPU preprocessing version of the read_ptr.

static std::atomic<int> s_sync_ticks;

static bool RunGpu(bool needs_simd_reset);
static void FlushGpu();

void DoState(PointerWrap& p)
{
  p.DoArray(s_video_buffer, FIFO_SIZE);
  u8* write_ptr = s_video_buffer_write_ptr;
  p.DoPointer(write_ptr, s_video_buffer);
  s_video_buffer_write_ptr = write_ptr;
  p.DoPointer(s_video_buffer_read_ptr, s_video_buffer);
  p.Do(s_sync_ticks);
}

void PauseAndLock(bool doLock, bool unpauseOnUnlock)
{
  if (doLock)
  {
    SyncGPU(SyncGPUReason::Other);
    EmulatorState(false);

    const SConfig& param = SConfig::GetInstance();

    if (!param.bCPUThread)
      return;

    s_gpu_mainloop.WaitYield(std::chrono::milliseconds(100), Host_YieldToUI);
  }
  else
  {
    if (unpauseOnUnlock)
      EmulatorState(true);
  }
}

void Init()
{
  // Padded so that SIMD overreads in the vertex loader are safe
  s_video_buffer = static_cast<u8*>(Common::AllocateMemoryPages(FIFO_SIZE + 4));
  ResetVideoBuffer();
  if (SConfig::GetInstance().bCPUThread)
    s_gpu_mainloop.Prepare();
  s_sync_ticks.store(0);
}

void Shutdown()
{
  if (s_gpu_mainloop.IsRunning())
    PanicAlert("Fifo shutting down while active");

  Common::FreeMemoryPages(s_video_buffer, FIFO_SIZE + 4);
  s_video_buffer = nullptr;
  s_video_buffer_write_ptr = nullptr;
  s_video_buffer_read_ptr = nullptr;
}

// May be executed from any thread, even the graphics thread.
// Created to allow for self shutdown.
void ExitGpuLoop()
{
  // This should break the wait loop in CPU thread
  CommandProcessor::fifo.bFF_GPReadEnable = false;
  s_gpu_mainloop.Wakeup();
  s_gpu_mainloop.Wait();

  // Terminate GPU thread loop
  s_emu_running_state.Set();
  s_gpu_mainloop.Stop(s_gpu_mainloop.kNonBlock);
}

void EmulatorState(bool running)
{
  s_emu_running_state.Set(running);
  if (running)
    s_gpu_mainloop.Wakeup();
  else
    s_gpu_mainloop.AllowSleep();
}

void SyncGPU(SyncGPUReason reason)
{
  if (reason == SyncGPUReason::Wraparound || reason == SyncGPUReason::CPRegisterAccess)
  {
    // add some ticks for wraparound
    s_sync_ticks.fetch_add(GPU_TIME_SLOT_SIZE);
    FlushGpu();
  }
  else if (reason == SyncGPUReason::Idle)
  {
    if (!s_gpu_mainloop.IsDone() || s_sync_ticks.load() > 0)
      s_gpu_mainloop.Wait();

    // For idle skip, or register reads, we want to ensure the GPU is completely finished.
    // The distance is always greater than the cycles, so this'll do.
    while (CanRunGpu())
    {
      s_sync_ticks.fetch_add(static_cast<int>(CommandProcessor::fifo.CPReadWriteDistance));
      FlushGpu();
    }
  }
  else
  {
    if (!s_gpu_mainloop.IsDone() || s_sync_ticks.load() > 0)
      s_gpu_mainloop.Wait();
  }

  // Ensure the CPU view is up to date.
  CommandProcessor::SetCPStatusFromCPU();
  CommandProcessor::UpdateInterrupts();
}

void PushFifoAuxBuffer(const void* ptr, size_t size)
{
#if 0
  if (size > (size_t)(s_fifo_aux_data + FIFO_SIZE - s_fifo_aux_write_ptr))
  {
    SyncGPU(SyncGPUReason::AuxSpace, /* may_move_read_ptr */ false);
    if (!s_gpu_mainloop.IsRunning())
    {
      // GPU is shutting down
      return;
    }
    if (size > (size_t)(s_fifo_aux_data + FIFO_SIZE - s_fifo_aux_write_ptr))
    {
      // That will sync us up to the last 32 bytes, so this short region
      // of FIFO would have to point to a 2MB display list or something.
      PanicAlert("absurdly large aux buffer");
      return;
    }
  }
  memcpy(s_fifo_aux_write_ptr, ptr, size);
  s_fifo_aux_write_ptr += size;
#endif
}

void* PopFifoAuxBuffer(size_t size)
{
#if 0
  void* ret = s_fifo_aux_read_ptr;
  s_fifo_aux_read_ptr += size;
  return ret;
#endif
  return 0;
}

// Description: RunGpuLoop() sends data through this function.
static void ReadDataFromFifo(u32 readPtr)
{
  size_t len = 32;
  if (len > (size_t)(s_video_buffer + FIFO_SIZE - s_video_buffer_write_ptr))
  {
    size_t existing_len = s_video_buffer_write_ptr - s_video_buffer_read_ptr;
    if (len > (size_t)(FIFO_SIZE - existing_len))
    {
      PanicAlert("FIFO out of bounds (existing %zu + new %zu > %u)", existing_len, len, FIFO_SIZE);
      return;
    }
    memmove(s_video_buffer, s_video_buffer_read_ptr, existing_len);
    s_video_buffer_write_ptr = s_video_buffer + existing_len;
    s_video_buffer_read_ptr = s_video_buffer;
  }
  // Copy new video instructions to s_video_buffer for future use in rendering the new picture
  Memory::CopyFromEmu(s_video_buffer_write_ptr, readPtr, len);
  s_video_buffer_write_ptr += len;
}

void ResetVideoBuffer()
{
  s_video_buffer_read_ptr = s_video_buffer;
  s_video_buffer_write_ptr = s_video_buffer;
}

bool CanRunGpu()
{
  CommandProcessor::SCPFifoStruct& fifo = CommandProcessor::fifo;
  return !CommandProcessor::IsInterruptWaiting() && fifo.bFF_GPReadEnable &&
         fifo.CPReadWriteDistance > 0 && !AtBreakpoint();
}

bool RunGpu(bool needs_simd_reset)
{
  int ticks_available = s_sync_ticks.load();
  if (ticks_available <= 0)
    return false;

  CommandProcessor::SetCPStatusFromGPU();
  // WARN_LOG(VIDEO, "Running %d ticks", ticks_available);

  bool did_any_work = false;
  while (CanRunGpu() && ticks_available > 0)
  {
    CommandProcessor::SCPFifoStruct& fifo = CommandProcessor::fifo;
    if (needs_simd_reset)
    {
      FPURoundMode::SaveSIMDState();
      FPURoundMode::LoadDefaultSIMDState();
      needs_simd_reset = false;
    }

    u32 cyclesExecuted = 0;
    u32 readPtr = fifo.CPReadPointer;
    ReadDataFromFifo(readPtr);

    if (readPtr == fifo.CPEnd)
      readPtr = fifo.CPBase;
    else
      readPtr += 32;

    ASSERT_MSG(COMMANDPROCESSOR, (s32)fifo.CPReadWriteDistance - 32 >= 0,
               "Negative fifo.CPReadWriteDistance = %i in FIFO Loop !\nThat can produce "
               "instability in the game. Please report it.",
               fifo.CPReadWriteDistance - 32);

    u8* write_ptr = s_video_buffer_write_ptr;
    s_video_buffer_read_ptr =
        OpcodeDecoder::Run(DataReader(s_video_buffer_read_ptr, write_ptr), &cyclesExecuted, false);

    Common::AtomicStore(fifo.CPReadPointer, readPtr);
    Common::AtomicAdd(fifo.CPReadWriteDistance, static_cast<u32>(-32));

    CommandProcessor::SetCPStatusFromGPU();
    s_sync_ticks.fetch_sub(static_cast<int>(cyclesExecuted));
    ticks_available -= static_cast<int>(cyclesExecuted);
    did_any_work = true;
  }

  // If we have ticks left over, remove them.
  // This way we don't leave a bunch of pending cycles.
  if (ticks_available > 0)
    s_sync_ticks.fetch_sub(ticks_available);

  return did_any_work;
}

// Description: Main FIFO update loop
// Purpose: Keep the Core HW updated about the CPU-GPU distance
static int s_gpu_mainloop_spin_count = 0;
void RunGpuLoop()
{
  AsyncRequests::GetInstance()->SetEnable(true);
  AsyncRequests::GetInstance()->SetPassthrough(false);

  s_gpu_mainloop.Run(
      [] {
        bool did_any_work = false;
        while (RunGpu(false))
          did_any_work = true;

        if (did_any_work)
        {
          s_gpu_mainloop_spin_count++;
          if (s_gpu_mainloop_spin_count == 100)
          {
            s_gpu_mainloop.AllowSleep();
            s_gpu_mainloop_spin_count = 0;
          }
          else
          {
            std::this_thread::yield();
          }
        }
        else
        {
          s_gpu_mainloop_spin_count = 0;
        }

        AsyncRequests::GetInstance()->PullEvents();
      },
      0);

  AsyncRequests::GetInstance()->SetEnable(false);
  AsyncRequests::GetInstance()->SetPassthrough(true);
}

void FlushGpu()
{
  if (SConfig::GetInstance().bCPUThread)
  {
    // Ensure the GPU thread is awake, and wait for it to finish all work.
    s_gpu_mainloop.Wakeup();
    s_gpu_mainloop.Wait();
    return;
  }

  // Single core mode - run pending cycles for the GPU on the CPU thread.
  if (RunGpu(true))
    FPURoundMode::LoadSIMDState();
}

void GpuMaySleep()
{
  if (SConfig::GetInstance().bCPUThread)
    s_gpu_mainloop.AllowSleep();
}

bool AtBreakpoint()
{
  CommandProcessor::SCPFifoStruct& fifo = CommandProcessor::fifo;
  return fifo.bFF_BPEnable && (fifo.CPReadPointer == fifo.CPBreakpoint);
}

void WakeGpuThread()
{
  if (!SConfig::GetInstance().bCPUThread)
    return;

  s_gpu_mainloop.Wakeup();
}

void UpdateWantDeterminism(bool want)
{
  // We are paused (or not running at all yet), so
  // it should be safe to change this.
  const SConfig& param = SConfig::GetInstance();
  bool gpu_thread = false;
  switch (param.m_GPUDeterminismMode)
  {
  case GPUDeterminismMode::Auto:
    gpu_thread = want;
    break;
  case GPUDeterminismMode::Disabled:
    gpu_thread = false;
    break;
  case GPUDeterminismMode::FakeCompletion:
    gpu_thread = true;
    break;
  }

  gpu_thread = gpu_thread && param.bCPUThread;

#if 0
  if (s_use_deterministic_gpu_thread != gpu_thread)
  {
    s_use_deterministic_gpu_thread = gpu_thread;
    if (gpu_thread)
    {
      // These haven't been updated in non-deterministic mode.
      s_video_buffer_seen_ptr = s_video_buffer_pp_read_ptr = s_video_buffer_read_ptr;
      CopyPreprocessCPStateFromMain();
      VertexLoaderManager::MarkAllDirty();
    }
  }
#endif
}

bool UseDeterministicGPUThread()
{
  // return s_use_deterministic_gpu_thread;
  return false;
}

static void SyncGPUCallback(u64 ticks, s64 cyclesLate)
{
  const auto& config = SConfig::GetInstance();

  // Run ticks cycles on the GPU.
  ticks += cyclesLate;
  if (CommandProcessor::fifo.CPReadWriteDistance > 0)
  {
    // int new_ticks = s_sync_ticks.fetch_add(static_cast<int>(ticks)) + static_cast<int>(ticks);
    s_sync_ticks.fetch_add(static_cast<int>(ticks));
    if (config.bCPUThread)
    {
      // Dual-core mode, ensure the GPU thread is awake, and not too far behind.
      s_gpu_mainloop.Wakeup();
    }
    else
    {
      if (RunGpu(true))
        FPURoundMode::LoadSIMDState();
    }
  }

  if (config.bSyncGPU && s_sync_ticks.load() >= config.iSyncGpuMaxDistance &&)
  {
    SyncGPU(SyncGPUReason::Other);
  }

  // Update CPU view of the command processor, and interrupts.
  CommandProcessor::SetCPStatusFromCPU();
  CommandProcessor::UpdateInterrupts();

  CoreTiming::ScheduleEvent(GPU_TIME_SLOT_SIZE, s_event_sync_gpu, GPU_TIME_SLOT_SIZE);
}

// Initialize GPU - CPU thread syncing, this gives us a deterministic way to start the GPU thread.
void Prepare()
{
  s_event_sync_gpu = CoreTiming::RegisterEvent("SyncGPUCallback", SyncGPUCallback);
  CoreTiming::ScheduleEvent(GPU_TIME_SLOT_SIZE, s_event_sync_gpu, GPU_TIME_SLOT_SIZE);
}
}  // namespace Fifo
