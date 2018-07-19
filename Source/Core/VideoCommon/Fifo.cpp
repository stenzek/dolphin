// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/Fifo.h"

#include <atomic>
#include <cstring>
#include <mutex>

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
static u8* s_video_buffer_read_ptr;   // Owned by video thread
static u8* s_video_buffer_write_ptr;  // Owned by video thread
static std::atomic<u32> s_video_buffer_size{};
static std::mutex s_video_buffer_lock;
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
static bool s_syncing_suspended;

static void FlushGpu();
static bool RunGpu(bool needs_simd_reset);

void DoState(PointerWrap& p)
{
  p.DoArray(s_video_buffer, FIFO_SIZE);
  p.DoPointer(s_video_buffer_write_ptr, s_video_buffer);
  p.DoPointer(s_video_buffer_read_ptr, s_video_buffer);
  p.Do(s_video_buffer_size);
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
  CommandProcessor::Run();

  if (reason == SyncGPUReason::Wraparound)
  {
    // add some ticks for wraparound
    s_sync_ticks.fetch_add(GPU_TIME_SLOT_SIZE);
  }
  else if (reason == SyncGPUReason::Idle || reason == SyncGPUReason::CPRegisterAccess)
  {
    // For idle skip, or register reads, we want to ensure the GPU is completely finished.
    // The distance is always greater than the cycles, so this'll do.
    s_sync_ticks.fetch_add(s_video_buffer_size.load());
  }

  FlushGpu();
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
void ReadDataFromFifo(u32 readPtr, size_t len)
{
  if (len > (size_t)(s_video_buffer + FIFO_SIZE - s_video_buffer_write_ptr))
  {
    std::lock_guard<std::mutex> guard(s_video_buffer_lock);
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
  s_video_buffer_size.fetch_add(static_cast<u32>(len));
}

void ResetVideoBuffer()
{
  FlushGpu();

  std::lock_guard<std::mutex> guard(s_video_buffer_lock);
  s_video_buffer_read_ptr = s_video_buffer;
  s_video_buffer_write_ptr = s_video_buffer;
}

bool RunGpu(bool needs_simd_reset)
{
  const auto& param = SConfig::GetInstance();

  int ticks_available;
  if (!param.bCPUThread || param.bSyncGPU)
  {
    ticks_available = s_sync_ticks.load();
    if (ticks_available <= 0)
      return false;
  }
  else
  {
    // In DC-mode, without SyncGPU, s_sync_ticks is not used.
    ticks_available = std::numeric_limits<int>::max();
  }

  bool did_any_work = false;
  bool simd_state_changed = false;
  while (s_video_buffer_write_ptr > s_video_buffer_read_ptr && ticks_available > 0)
  {
    if (needs_simd_reset)
    {
      FPURoundMode::SaveSIMDState();
      FPURoundMode::LoadDefaultSIMDState();
      needs_simd_reset = false;
      simd_state_changed = true;
    }

    u32 cyclesExecuted = 0;
    u8* old_read_ptr = s_video_buffer_read_ptr;
    s_video_buffer_read_ptr = OpcodeDecoder::Run(
        DataReader(s_video_buffer_read_ptr, s_video_buffer_write_ptr), &cyclesExecuted, false);
    u32 bytes_consumed = u32(s_video_buffer_read_ptr - old_read_ptr);
    if (bytes_consumed == 0)
      break;

    s_video_buffer_size.fetch_sub(bytes_consumed);
    s_sync_ticks.fetch_sub(static_cast<int>(cyclesExecuted));
    ticks_available -= static_cast<int>(cyclesExecuted);
    did_any_work = true;
  }

  // If we have ticks left over, remove them.
  // This way we don't leave a bunch of pending cycles.
  if (ticks_available > 0)
    s_sync_ticks.fetch_sub(ticks_available);

  if (simd_state_changed)
    FPURoundMode::LoadSIMDState();

  return did_any_work;
}

// Description: Main FIFO update loop
// Purpose: Keep the Core HW updated about the CPU-GPU distance
void RunGpuLoop()
{
  AsyncRequests::GetInstance()->SetEnable(true);
  AsyncRequests::GetInstance()->SetPassthrough(false);

  s_gpu_mainloop.Run(
      [] {
        // Skip mutex lock if there is no commands queued.
        bool did_any_work = false;
        if (s_video_buffer_size.load() > 0)
        {
          std::lock_guard<std::mutex> guard(s_video_buffer_lock);
          did_any_work = RunGpu(false);
        }

        // We likely won't have any work for a while, so ensure the pipeline is flushed.
        if (did_any_work)
          g_vertex_manager->Flush();
        else
          GpuMaySleep();

        AsyncRequests::GetInstance()->PullEvents();
      },
      0);

  AsyncRequests::GetInstance()->SetEnable(false);
  AsyncRequests::GetInstance()->SetPassthrough(true);
}

void FlushGpu()
{
  if (s_video_buffer_size.load() == 0)
    return;

  if (SConfig::GetInstance().bCPUThread)
  {
    s_gpu_mainloop.Wait();
    return;
  }

  // Single core mode - run pending cycles for the GPU on the CPU thread.
  RunGpu(true);
}

void GpuMaySleep()
{
  if (SConfig::GetInstance().bCPUThread)
    s_gpu_mainloop.AllowSleep();
}

void WakeGpu()
{
  if (SConfig::GetInstance().bCPUThread)
    s_gpu_mainloop.Wakeup();

  // if the sync GPU callback is suspended, wake it up.
  if (!SConfig::GetInstance().bCPUThread || SConfig::GetInstance().bSyncGPU)
  {
    if (s_syncing_suspended)
    {
      s_syncing_suspended = false;
      CoreTiming::ScheduleEvent(GPU_TIME_SLOT_SIZE, s_event_sync_gpu, GPU_TIME_SLOT_SIZE);
    }
  }
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

static int RunGpuOnCpu(int ticks)
{
  s_sync_ticks.fetch_add(static_cast<int>(ticks * SConfig::GetInstance().fSyncGpuOverclock));
  RunGpu(true);

  if (s_video_buffer_size.load() == 0)
    return -1;
  else
    return -s_sync_ticks.load() + GPU_TIME_SLOT_SIZE;
}

/* This function checks the emulated CPU - GPU distance and may wake up the GPU,
 * or block the CPU if required. It should be called by the CPU thread regularly.
 * @ticks The gone emulated CPU time.
 * @return A good time to call WaitForGpuThread() next.
 */
static int WaitForGpuThread(int ticks)
{
  const SConfig& param = SConfig::GetInstance();

  int old = s_sync_ticks.fetch_add(ticks);
  int now = old + ticks;

  // GPU is idle, so stop polling.
  if (old >= 0 && s_gpu_mainloop.IsDone())
    return -1;

  // Wakeup GPU
  if (old < param.iSyncGpuMinDistance && now >= param.iSyncGpuMinDistance)
    WakeGpu();

  // If the GPU is still sleeping, wait for a longer time
  if (now < param.iSyncGpuMinDistance)
    return GPU_TIME_SLOT_SIZE + param.iSyncGpuMinDistance - now;

  // Wait for GPU
  if (now >= param.iSyncGpuMaxDistance)
    s_gpu_mainloop.Wait();

  return GPU_TIME_SLOT_SIZE;
}

static void SyncGPUCallback(u64 ticks, s64 cyclesLate)
{
  ticks += cyclesLate;
  int next = -1;

  if (!SConfig::GetInstance().bCPUThread)
    next = RunGpuOnCpu((int)ticks);
  else if (SConfig::GetInstance().bSyncGPU)
    next = WaitForGpuThread((int)ticks);

  s_syncing_suspended = next < 0;
  if (!s_syncing_suspended)
    CoreTiming::ScheduleEvent(next, s_event_sync_gpu, next);
}

// Initialize GPU - CPU thread syncing, this gives us a deterministic way to start the GPU thread.
void Prepare()
{
  s_event_sync_gpu = CoreTiming::RegisterEvent("SyncGPUCallback", SyncGPUCallback);
  CoreTiming::ScheduleEvent(GPU_TIME_SLOT_SIZE, s_event_sync_gpu, GPU_TIME_SLOT_SIZE);
}
}  // namespace Fifo
