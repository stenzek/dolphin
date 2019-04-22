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

#define FIFO_DEBUG_LOG(...) WARN_LOG(VIDEO, __VA_ARGS__)

#ifndef FIFO_DEBUG_LOG
#define FIFO_DEBUG_LOG(...)                                                                        \
  do                                                                                               \
  {                                                                                                \
  } while (0)
#endif

namespace Fifo
{
static Common::BlockingLoop s_gpu_mainloop;

static Common::Flag s_emu_running_state;

// Most of this array is unlikely to be faulted in...
static u8 s_fifo_aux_data[FIFO_SIZE];
static u8* s_fifo_aux_write_ptr;
static u8* s_fifo_aux_read_ptr;

// This could be in SConfig, but it depends on multiple settings
// and can change at runtime.
static bool s_use_deterministic_gpu_thread;

static CoreTiming::EventType* s_event_sync_gpu;

// STATE_TO_SAVE
u8* video_buffer;
u8* video_buffer_read_ptr;           // owned by GPU thread
u8* video_buffer_write_ptr;          // owned by CPU thread
std::atomic<u32> video_buffer_size;  // modified by both threads

// In deterministic dual core mode, the pp size is the size available to the GPU thread.
u8* video_buffer_pp_read_ptr;
std::atomic<u32> video_buffer_pp_size;
std::mutex video_buffer_mutex;

static std::atomic<int> s_sync_ticks;
static bool s_gpu_suspended;
static std::atomic_bool s_gpu_idle;
static Common::Event s_sync_wakeup_event;

static void RunGpuSingleCore(bool allow_run_ahead);
static void RunGpuDualCore();

void DoState(PointerWrap& p)
{
  p.DoArray(video_buffer, FIFO_SIZE);
  p.DoPointer(video_buffer_read_ptr, video_buffer);
  p.DoPointer(video_buffer_write_ptr, video_buffer);
  p.Do(video_buffer_size);

  if (p.mode == PointerWrap::MODE_READ && s_use_deterministic_gpu_thread)
  {
    // We're good and paused, right?
    video_buffer_pp_read_ptr = video_buffer_read_ptr;
    video_buffer_pp_size.store(video_buffer_size.load());
  }

  p.Do(s_sync_ticks);
  p.Do(s_gpu_idle);

  if (p.GetMode() == PointerWrap::MODE_READ)
  {
    // By updating the suspend event here, this may not be completely deterministic when loading
    // save states, as some GPU cycles may get executed earlier in the load, vs when it was saved.
    UpdateGPUSuspendState();
  }
}

void PauseAndLock(bool doLock, bool unpauseOnUnlock)
{
  if (doLock)
  {
    SyncGPU(SyncGPUReason::Other);
    EmulatorState(false);

    const SConfig& param = SConfig::GetInstance();

    if (!param.bCPUThread || s_use_deterministic_gpu_thread)
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
  video_buffer = static_cast<u8*>(Common::AllocateMemoryPages(FIFO_SIZE + 4));
  ResetVideoBuffer();
  if (SConfig::GetInstance().bCPUThread)
    s_gpu_mainloop.Prepare();
  s_sync_ticks.store(0);
}

void Shutdown()
{
  if (s_gpu_mainloop.IsRunning())
    PanicAlert("Fifo shutting down while active");

  Common::FreeMemoryPages(video_buffer, FIFO_SIZE + 4);
  video_buffer = nullptr;
  video_buffer_read_ptr = nullptr;
  video_buffer_write_ptr = nullptr;
  video_buffer_size.store(0);
  video_buffer_pp_read_ptr = nullptr;
  video_buffer_pp_size.store(0);
}

// May be executed from any thread, even the graphics thread.
// Created to allow for self shutdown.
void ExitGpuLoop()
{
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

bool IsGPUIdle()
{
  return s_gpu_idle.load();
}

void SyncGPU(SyncGPUReason reason)
{
  if (!SConfig::GetInstance().bCPUThread || !s_use_deterministic_gpu_thread)
    return;

  s_gpu_mainloop.Wait();
  if (!s_gpu_mainloop.IsRunning())
    return;

  // Opportunistically reset FIFOs so we don't wrap around.
  if (s_fifo_aux_write_ptr != s_fifo_aux_read_ptr)
    PanicAlert("aux fifo not synced (%p, %p)", s_fifo_aux_write_ptr, s_fifo_aux_read_ptr);

  memmove(s_fifo_aux_data, s_fifo_aux_read_ptr, s_fifo_aux_write_ptr - s_fifo_aux_read_ptr);
  s_fifo_aux_write_ptr -= (s_fifo_aux_read_ptr - s_fifo_aux_data);
  s_fifo_aux_read_ptr = s_fifo_aux_data;
}

void PushFifoAuxBuffer(const void* ptr, size_t size)
{
  if (size > (size_t)(s_fifo_aux_data + FIFO_SIZE - s_fifo_aux_write_ptr))
  {
    SyncGPU(SyncGPUReason::AuxSpace);
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
}

void* PopFifoAuxBuffer(size_t size)
{
  void* ret = s_fifo_aux_read_ptr;
  s_fifo_aux_read_ptr += size;
  return ret;
}

void SyncGPUForRegisterAccess(bool is_write)
{
  FIFO_DEBUG_LOG("Syncing for register %s", is_write ? "write" : "read");
  // Don't wake the thread if we're not writing, and idle.
  if (s_gpu_suspended && !is_write)
    return;

  if (SConfig::GetInstance().bCPUThread)
    RunGpuDualCore();
  else
    RunGpuSingleCore(false);

  UpdateGPUSuspendState();
}

void ResetVideoBuffer()
{
  FIFO_DEBUG_LOG("Resetting video buffer");
  if (!SConfig::GetInstance().bCPUThread)
    s_gpu_mainloop.Wait();

  video_buffer_read_ptr = video_buffer;
  video_buffer_write_ptr = video_buffer;
  video_buffer_size.store(0);
  video_buffer_pp_read_ptr = video_buffer;
  video_buffer_pp_size.store(0);
}

// Description: Main FIFO update loop
// Purpose: Keep the Core HW updated about the CPU-GPU distance
void RunGpuLoop()
{
  AsyncRequests::GetInstance()->SetEnable(true);
  AsyncRequests::GetInstance()->SetPassthrough(false);

  s_gpu_mainloop.Run(
      [] {
        const SConfig& param = SConfig::GetInstance();

        // Do nothing while paused
        if (!s_emu_running_state.IsSet())
          return;

        u8*& read_ptr =
            s_use_deterministic_gpu_thread ? video_buffer_pp_read_ptr : video_buffer_read_ptr;
        auto& buffer_size =
            s_use_deterministic_gpu_thread ? video_buffer_pp_size : video_buffer_size;

        AsyncRequests::GetInstance()->PullEvents();

        // check if we are able to run this buffer
        u32 bytes_remaining;
        while ((bytes_remaining = buffer_size.load()) > 0)
        {
          if (param.bSyncGPU && s_sync_ticks.load() < param.iSyncGpuMinDistance)
            break;

          u32 cyclesExecuted, bytesExecuted;
          {
            std::lock_guard<std::mutex> guard(video_buffer_mutex);
            u8* old_read_ptr = read_ptr;
            u8* new_read_ptr = OpcodeDecoder::Run(
                DataReader(old_read_ptr, old_read_ptr + bytes_remaining), &cyclesExecuted, false);
            bytesExecuted = static_cast<u32>(new_read_ptr - old_read_ptr);
            if (bytesExecuted == 0)
              break;

            read_ptr = new_read_ptr;
            video_buffer_size.fetch_sub(bytesExecuted);
          }

          if (param.bSyncGPU)
          {
            cyclesExecuted = (int)(cyclesExecuted / param.fSyncGpuOverclock);
            int old = s_sync_ticks.fetch_sub(cyclesExecuted);
            if (old >= param.iSyncGpuMaxDistance &&
                old - (int)cyclesExecuted < param.iSyncGpuMaxDistance)
              s_sync_wakeup_event.Set();
          }

          // This call is pretty important in DualCore mode and must be called in the FIFO Loop.
          // If we don't, s_swapRequested or s_efbAccessRequested won't be set to false
          // leading the CPU thread to wait in Video_BeginField or Video_AccessEFB thus slowing
          // things down.
          AsyncRequests::GetInstance()->PullEvents();

          // fast skip remaining GPU time if fifo is empty
          if (s_sync_ticks.load() > 0)
          {
            int old = s_sync_ticks.exchange(0);
            if (old >= param.iSyncGpuMaxDistance)
              s_sync_wakeup_event.Set();
          }

          // The fifo is empty and it's unlikely we will get any more work in the near future.
          // Make sure VertexManager finishes drawing any primitives it has stored in it's buffer.
          g_vertex_manager->Flush();
        }

        // If we reach here and the bytes remaining are greater than zero, the GPU is waiting for
        // data, or has exceeded its cycle limit.
        const bool gpu_idle = buffer_size.load() > 0;
        s_gpu_idle.store(gpu_idle);
      },
      100);

  AsyncRequests::GetInstance()->SetEnable(false);
  AsyncRequests::GetInstance()->SetPassthrough(true);
}

void FlushGpu()
{
  const SConfig& param = SConfig::GetInstance();

  if (!param.bCPUThread || s_use_deterministic_gpu_thread)
    return;

  s_gpu_mainloop.Wait();
}

void WakeGpu()
{
  if (!SConfig::GetInstance().bCPUThread)
    return;

  s_gpu_mainloop.Wakeup();
}

void GpuMaySleep()
{
  s_gpu_mainloop.AllowSleep();
}

void RunGpu(bool allow_run_ahead)
{
  const SConfig& param = SConfig::GetInstance();
  if (!param.bCPUThread)
    RunGpuSingleCore(allow_run_ahead);
  else
    RunGpuDualCore();

  UpdateGPUSuspendState();
}

void RunGpuSingleCore(bool allow_run_ahead)
{
#if 0
  // Single core - run as many ticks as we have available, stall once we run out.
  //s32 available_ticks = s_sync_ticks.load();
  s32 available_ticks = std::numeric_limits<s32>::max();
  if (!allow_run_ahead && available_ticks <= 0)
  {
    // We can't read from the FIFO, or execute any commands.
    // None of the statements below will execute.
    return;
  }

  FPURoundMode::SaveSIMDState();
  FPURoundMode::LoadDefaultSIMDState();

  FIFO_DEBUG_LOG("running GPU for %u ticks", available_ticks);

  u32 total_bytes_used = 0;
  s32 total_cycles_used = 0;
  s_gpu_idle.store(false);

  while (total_cycles_used < available_ticks || allow_run_ahead)
  {
    // Read as much as possible from the FIFO.
    // We could read in smaller batches, to make the RW pointer more accurate to the actual
    // GPU progress, but since our timings are garbage anyway, it's unlikely to make much of
    // a difference. Larger batches means fewer loops, and lower CPU usage.
    // u32 bytes_copied = CommandProcessor::CopyToVideoBuffer(FIFO_EXECUTE_THRESHOLD_SIZE);
    u32 bytes_copied = CommandProcessor::CopyToVideoBuffer(32);

    // Execute as many commands as possible.
    u32 cyclesExecuted = 0;
    u8* old_read_ptr = video_buffer_read_ptr;
    video_buffer_read_ptr = OpcodeDecoder::Run(
        DataReader(video_buffer_read_ptr, video_buffer_write_ptr), &cyclesExecuted, false);

    u32 bytes_consumed = u32(video_buffer_read_ptr - old_read_ptr);
    if (bytes_copied == 0 && bytes_consumed == 0)
    {
      // Waiting for more data.
      s_gpu_idle.store(true);
      break;
    }

    total_bytes_used += bytes_consumed;
    total_cycles_used += static_cast<s32>(cyclesExecuted);

    FIFO_DEBUG_LOG("used %u/%u bytes, %d/%d cycles", total_bytes_used, video_buffer_size.load(),
                   total_cycles_used, s_sync_ticks.load());
  }

  if (total_cycles_used >= available_ticks)
    FIFO_DEBUG_LOG("out of ticks %u/%u", total_cycles_used, available_ticks);

  video_buffer_size.fetch_sub(total_bytes_used);
  //s_sync_ticks.fetch_sub(total_cycles_used);
  s_sync_ticks.store(0);

  FPURoundMode::LoadSIMDState();
#else
  s_gpu_idle.store(true);
  s_sync_ticks.store(0);
  if (CommandProcessor::CopyToVideoBuffer(FIFO_SIZE) == 0)
    return;

  FPURoundMode::SaveSIMDState();
  FPURoundMode::LoadDefaultSIMDState();

  u32 cyclesExecuted = 0;
  u8* old_read_ptr = video_buffer_read_ptr;
  video_buffer_read_ptr = OpcodeDecoder::Run(
    DataReader(video_buffer_read_ptr, video_buffer_write_ptr), &cyclesExecuted, false);
  u32 bytes_consumed = u32(video_buffer_read_ptr - old_read_ptr);
  video_buffer_size.fetch_sub(bytes_consumed);
  s_gpu_idle.store(false);

  FPURoundMode::LoadSIMDState();
#endif
}

void RunGpuDualCore()
{
  // Dual core - simulate an "infinitely fast" GPU, copying as much as possible to the video buffer,
  // and executing it on the GPU thread.
  u32 bytes_copied = CommandProcessor::CopyToVideoBuffer(FIFO_SIZE);
  if (bytes_copied == 0)
  {
    // Nothing new copied, so don't bother waking the GPU thread.
    return;
  }

  // Wake the GPU thread, as there is new work.
  // We defer the wake until the next SyncGPU if it is enabled, and there are no ticks. Otherwise,
  // the GPU thread would wake, immediately sleep, and then wake again once ticks were added.
  const bool at_breakpoint = CommandProcessor::AtBreakpoint();
  if (at_breakpoint || !SConfig::GetInstance().bSyncGPU ||
      s_sync_ticks.load() > SConfig::GetInstance().iSyncGpuMinDistance)
  {
    s_gpu_mainloop.Wakeup();
  }

  // Synchronize with the GPU thread when we hit a FIFO breakpoint.
  if (at_breakpoint)
    s_gpu_mainloop.Wait();

  s_gpu_mainloop.Wakeup();
  s_gpu_mainloop.Wait();
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

  if (s_use_deterministic_gpu_thread != gpu_thread)
  {
    s_use_deterministic_gpu_thread = gpu_thread;
    if (gpu_thread)
    {
      // These haven't been updated in non-deterministic mode.
      video_buffer_pp_read_ptr = video_buffer_read_ptr;
      video_buffer_pp_size.store(video_buffer_size.load());
      CopyPreprocessCPStateFromMain();
      VertexLoaderManager::MarkAllDirty();
    }
  }
}

bool UseDeterministicGPUThread()
{
  return s_use_deterministic_gpu_thread;
}

/* This function checks the emulated CPU - GPU distance and may wake up the GPU,
 * or block the CPU if required. It should be called by the CPU thread regularly.
 * @ticks The gone emulated CPU time.
 * @return A good time to call WaitForGpuThread() next.
 */
static int WaitForGpuThread(int ticks)
{
  const SConfig& param = SConfig::GetInstance();

  // If we don't read from the FIFO here, we could stall forever if <512 bytes are written.
  RunGpuDualCore();

  int old = s_sync_ticks.fetch_add(ticks);
  int now = old + ticks;

  // GPU is idle, so stop polling.
  if (old >= 0 && s_gpu_mainloop.IsDone() && s_gpu_idle)
    return -1;

  // Wakeup GPU
  if (old < param.iSyncGpuMinDistance && now >= param.iSyncGpuMinDistance)
    s_gpu_mainloop.Wakeup();

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
  const auto& param = SConfig::GetInstance();
  int ticks_to_add = static_cast<int>((ticks + cyclesLate) * param.fSyncGpuOverclock);
  int next = -1;

  if (!param.bCPUThread)
  {
    s_sync_ticks.fetch_add(ticks_to_add);
    RunGpuSingleCore(false);

    // Cancel the event if the GPU is now idle.
    //next = s_gpu_idle ? -1 : (-s_sync_ticks.load() + GPU_TIME_SLOT_SIZE);
  }
  else if (param.bSyncGPU)
  {
    next = WaitForGpuThread(ticks);
  }
  else
  {
    RunGpuDualCore();
  }

  FIFO_DEBUG_LOG("SyncGPUCallback: VBS=%u", video_buffer_size.load());

  s_gpu_suspended = next < 0;
  if (!s_gpu_suspended)
    CoreTiming::ScheduleEvent(next, s_event_sync_gpu, next);
  else
  {
    FIFO_DEBUG_LOG("Syncing disabled");
    ASSERT(!CommandProcessor::CanReadFromFifo());
  }
}

void UpdateGPUSuspendState()
{
  bool gpu_suspended;
  int next = GPU_TIME_SLOT_SIZE;
  if (!SConfig::GetInstance().bCPUThread)
  {
    // For single-core, we need the event if there is a non-empty FIFO, or a non-empty video buffer.
    gpu_suspended = !CommandProcessor::CanReadFromFifo() && s_gpu_idle;
    next = std::max(GPU_TIME_SLOT_SIZE - s_sync_ticks.load(), 1);
  }
  else if (SConfig::GetInstance().bSyncGPU)
  {
    // For SyncGPU, the GPU thread must be busy.
    gpu_suspended = !CommandProcessor::CanReadFromFifo() && s_gpu_mainloop.IsDone() && s_gpu_idle;
  }
  else
  {
    // Pure dualcore - we only need to sync (read from the FIFO) when there is data.
    gpu_suspended = !CommandProcessor::CanReadFromFifo();
  }

  if (gpu_suspended != s_gpu_suspended)
  {
    FIFO_DEBUG_LOG("syncing %s", gpu_suspended ? "disabled" : "enabled");

    if (!gpu_suspended)
      CoreTiming::ScheduleEvent(next, s_event_sync_gpu, next);
    else
    {
      CoreTiming::RemoveEvent(s_event_sync_gpu);
      ASSERT(!CommandProcessor::CanReadFromFifo());
    }

    s_gpu_suspended = gpu_suspended;
  }
}

// Initialize GPU - CPU thread syncing, this gives us a deterministic way to start the GPU thread.
void Prepare()
{
  s_event_sync_gpu = CoreTiming::RegisterEvent("SyncGPUCallback", SyncGPUCallback);
  s_gpu_suspended = true;
}
}  // namespace Fifo
