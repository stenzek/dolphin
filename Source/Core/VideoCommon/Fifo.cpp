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
#include "Core/HW/GPFifo.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/MMIOHandlers.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/Host.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Fifo
{
// This constant controls the size of the video buffer, which is where the data from the FIFO
// located in guest memory is copied to. GPU commands are executed from this buffer.
static constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;

// This constant controls the execution threshold for the GPU. In single core mode, this defines
// the size of the batches which are copied out of guest memory to the video buffer. In dual core
// more, batches will be copied to the video buffer for execution on the GPU thread every time
// the FIFO reaches this size.
static constexpr u32 FIFO_EXECUTE_THRESHOLD_SIZE = 512;

// This constant controls the interval at which the GPU will be executed in single-core mode, or
// synchronized with the GPU thread in dual-core mode.
static constexpr int GPU_TIME_SLOT_SIZE = 1000;

static Common::BlockingLoop s_gpu_mainloop;

// STATE_TO_SAVE
SCPFifoStruct fifo;
static UCPStatusReg m_CPStatusReg;
static UCPCtrlReg m_CPCtrlReg;
static UCPClearReg m_CPClearReg;

static u16 m_bboxleft;
static u16 m_bboxtop;
static u16 m_bboxright;
static u16 m_bboxbottom;
static u16 m_tokenReg;

static Common::Flag s_interrupt_set;

// Video buffer. This contains a copy of the FIFO data in one contiguous buffer, suitable for vertex
// loading. s_video_buffer_read_ptr is owned by the GPU thread, and the write_ptr is owned by the
// CPU thread in dual core mode. The lock provides synchronization when the buffer needs to be moved
// back to the front to create additional space.
static u8* s_video_buffer;
static u8* s_video_buffer_read_ptr;
static u8* s_video_buffer_write_ptr;
static std::atomic<u32> s_video_buffer_size{};
static std::mutex s_video_buffer_lock;

static std::atomic<s32> s_sync_ticks;
static CoreTiming::EventType* s_event_sync_gpu;
static bool s_syncing_suspended;

// Checking whether we can run, due to breakpoints and R/W distance.
static bool AtBreakpoint();
static bool CanReadFromFifo();
static bool GpuIsIdle();

// Raises interrupts based on FIFO state.
static void UpdateInterrupts();

// "Wakes" the GPU, by scheduling the sync event.
static void WakeGpu();

// Reads from the FIFO and processes commands.
static void RunGpuSingleCore();
static void RunGpuDualCore();

// Gives a ticks boost to the GPU, to execute any commands inbetween JIT blocks/sync events.
static void SyncForRegisterAccess();

// Copies up to maximum_len bytes to the video buffer.
static void CopyToVideoBuffer(u32 maximum_copy_size);

// Updates the SyncGPU events tate.
static void UpdateSyncEvent();

// Updating registers with FIFO state.
static void SetCpClearRegister();
static void SetCpControlRegister();
static void SetCpStatusRegister();

void SCPFifoStruct::DoState(PointerWrap& p)
{
  p.Do(CPBase);
  p.Do(CPEnd);
  p.Do(CPHiWatermark);
  p.Do(CPLoWatermark);
  p.Do(CPReadWriteDistance);
  p.Do(CPWritePointer);
  p.Do(CPReadPointer);
  p.Do(CPBreakpoint);

  p.Do(bFF_GPLinkEnable);
  p.Do(bFF_GPReadEnable);
  p.Do(bFF_BPEnable);

  p.Do(bFF_BPInt);
  p.Do(bFF_LoWatermarkInt);
  p.Do(bFF_HiWatermarkInt);

  p.Do(bFF_Breakpoint);
  p.Do(bFF_LoWatermark);
  p.Do(bFF_HiWatermark);
}

void DoState(PointerWrap& p)
{
  p.DoPOD(m_CPStatusReg);
  p.DoPOD(m_CPCtrlReg);
  p.DoPOD(m_CPClearReg);
  p.Do(m_bboxleft);
  p.Do(m_bboxtop);
  p.Do(m_bboxright);
  p.Do(m_bboxbottom);
  p.Do(m_tokenReg);
  fifo.DoState(p);

  p.Do(s_interrupt_set);

  p.DoArray(s_video_buffer, FIFO_SIZE);
  p.DoPointer(s_video_buffer_write_ptr, s_video_buffer);
  p.DoPointer(s_video_buffer_read_ptr, s_video_buffer);
  p.Do(s_video_buffer_size);
  p.Do(s_sync_ticks);
}

static inline void WriteLow(volatile u32& _reg, u16 lowbits)
{
  Common::AtomicStore(_reg, (_reg & 0xFFFF0000) | lowbits);
}
static inline void WriteHigh(volatile u32& _reg, u16 highbits)
{
  Common::AtomicStore(_reg, (_reg & 0x0000FFFF) | ((u32)highbits << 16));
}
static inline u16 ReadLow(u32 _reg)
{
  return (u16)(_reg & 0xFFFF);
}
static inline u16 ReadHigh(u32 _reg)
{
  return (u16)(_reg >> 16);
}

void PauseAndLock(bool doLock, bool unpauseOnUnlock)
{
  if (doLock)
  {
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

void EmulatorState(bool running)
{
  if (running)
    s_gpu_mainloop.Wakeup();
  else
    s_gpu_mainloop.AllowSleep();
}

void Init()
{
  m_CPStatusReg.Hex = 0;
  m_CPStatusReg.CommandIdle = 1;
  m_CPStatusReg.ReadIdle = 1;

  m_CPCtrlReg.Hex = 0;

  m_CPClearReg.Hex = 0;

  m_bboxleft = 0;
  m_bboxtop = 0;
  m_bboxright = 640;
  m_bboxbottom = 480;

  m_tokenReg = 0;

  memset(&fifo, 0, sizeof(fifo));
  fifo.bFF_Breakpoint = 0;
  fifo.bFF_HiWatermark = 0;
  fifo.bFF_HiWatermarkInt = 0;
  fifo.bFF_LoWatermark = 0;
  fifo.bFF_LoWatermarkInt = 0;

  s_interrupt_set.Clear();

  // Padded so that SIMD overreads in the vertex loader are safe
  s_video_buffer = static_cast<u8*>(Common::AllocateMemoryPages(FIFO_SIZE + 4));
  s_video_buffer_read_ptr = s_video_buffer;
  s_video_buffer_write_ptr = s_video_buffer;
  s_video_buffer_size.store(0);
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
  s_gpu_mainloop.Stop(s_gpu_mainloop.kNonBlock);
}

void WakeGpuThread()
{
  s_gpu_mainloop.Wakeup();
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // Timing and metrics MMIOs are stubbed with fixed values.
  struct
  {
    u32 addr;
    u16 value;
  } metrics_mmios[] = {
      {XF_RASBUSY_L, 0},
      {XF_RASBUSY_H, 0},
      {XF_CLKS_L, 0},
      {XF_CLKS_H, 0},
      {XF_WAIT_IN_L, 0},
      {XF_WAIT_IN_H, 0},
      {XF_WAIT_OUT_L, 0},
      {XF_WAIT_OUT_H, 0},
      {VCACHE_METRIC_CHECK_L, 0},
      {VCACHE_METRIC_CHECK_H, 0},
      {VCACHE_METRIC_MISS_L, 0},
      {VCACHE_METRIC_MISS_H, 0},
      {VCACHE_METRIC_STALL_L, 0},
      {VCACHE_METRIC_STALL_H, 0},
      {CLKS_PER_VTX_OUT, 4},
  };
  for (auto& metrics_mmio : metrics_mmios)
  {
    mmio->Register(base | metrics_mmio.addr, MMIO::Constant<u16>(metrics_mmio.value),
                   MMIO::InvalidWrite<u16>());
  }

  mmio->Register(base | FIFO_TOKEN_REGISTER, MMIO::DirectRead<u16>(&m_tokenReg),
                 MMIO::DirectWrite<u16>(&m_tokenReg));

  // Bounding box registers are read only.
  mmio->Register(base | FIFO_BOUNDING_BOX_LEFT, MMIO::DirectRead<u16>(&m_bboxleft),
                 MMIO::InvalidWrite<u16>());
  mmio->Register(base | FIFO_BOUNDING_BOX_RIGHT, MMIO::DirectRead<u16>(&m_bboxright),
                 MMIO::InvalidWrite<u16>());
  mmio->Register(base | FIFO_BOUNDING_BOX_TOP, MMIO::DirectRead<u16>(&m_bboxtop),
                 MMIO::InvalidWrite<u16>());
  mmio->Register(base | FIFO_BOUNDING_BOX_BOTTOM, MMIO::DirectRead<u16>(&m_bboxbottom),
                 MMIO::InvalidWrite<u16>());

  mmio->Register(base | STATUS_REGISTER, MMIO::ComplexRead<u16>([](u32) {
                   SyncForRegisterAccess();
                   SetCpStatusRegister();
                   return m_CPStatusReg.Hex;
                 }),
                 MMIO::InvalidWrite<u16>());

  mmio->Register(base | CTRL_REGISTER, MMIO::DirectRead<u16>(&m_CPCtrlReg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   UCPCtrlReg tmp(val);
                   m_CPCtrlReg.Hex = tmp.Hex;
                   SetCpControlRegister();
                   // WARN_LOG(VIDEO, "CPRWD: %u, read=%s", fifo.CPReadWriteDistance,
                   // fifo.bFF_GPReadEnable ? "yes" : "no");
                   WakeGpu();
                 }));

  mmio->Register(base | CLEAR_REGISTER, MMIO::DirectRead<u16>(&m_CPClearReg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   UCPClearReg tmp(val);
                   m_CPClearReg.Hex = tmp.Hex;
                   SetCpClearRegister();
                 }));

  mmio->Register(base | PERF_SELECT, MMIO::InvalidRead<u16>(), MMIO::Nop<u16>());

  mmio->Register(base | FIFO_BASE_LO, MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPBase)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteLow(fifo.CPBase, val & 0xFFE0);
                 }));

  mmio->Register(base | FIFO_BASE_HI, MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPBase)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteHigh(fifo.CPBase, val);
                 }));

  mmio->Register(base | FIFO_END_LO, MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPEnd)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteLow(fifo.CPEnd, val & 0xFFE0);
                 }));

  mmio->Register(base | FIFO_END_HI, MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPEnd)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteHigh(fifo.CPEnd, val);
                 }));

  mmio->Register(base | FIFO_BP_LO, MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPBreakpoint)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteLow(fifo.CPBreakpoint, val & 0xffe0);
                 }));
  mmio->Register(base | FIFO_BP_HI,
                 MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPBreakpoint)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteHigh(fifo.CPBreakpoint, val);
                 }));

  mmio->Register(base | FIFO_LO_WATERMARK_LO,
                 MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPLoWatermark)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteLow(fifo.CPLoWatermark, val);
                 }));

  mmio->Register(base | FIFO_LO_WATERMARK_HI,
                 MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPLoWatermark)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteHigh(fifo.CPLoWatermark, val);
                 }));

  mmio->Register(base | FIFO_HI_WATERMARK_LO,
                 MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPHiWatermark)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteLow(fifo.CPHiWatermark, val);
                 }));

  mmio->Register(base | FIFO_HI_WATERMARK_HI,
                 MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPHiWatermark)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteHigh(fifo.CPHiWatermark, val);
                 }));

  // Some MMIOs have different handlers for single core vs. dual core mode.
  mmio->Register(base | FIFO_RW_DISTANCE_LO, MMIO::ComplexRead<u16>([](u32) {
                   SyncForRegisterAccess();
                   return ReadLow(fifo.CPReadWriteDistance);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteLow(fifo.CPReadWriteDistance, val & 0xFFE0);
                 }));
  mmio->Register(base | FIFO_RW_DISTANCE_HI, MMIO::ComplexRead<u16>([](u32) {
                   SyncForRegisterAccess();
                   return ReadHigh(fifo.CPReadWriteDistance);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteHigh(fifo.CPReadWriteDistance, val);

                   // TODO: Is this correct? BE so this should be on the hi not low?
                   if (fifo.CPReadWriteDistance == 0)
                     GPFifo::ResetGatherPipe();

                   WakeGpu();
                 }));
  mmio->Register(base | FIFO_READ_POINTER_LO, MMIO::ComplexRead<u16>([](u32) {
                   SyncForRegisterAccess();
                   return ReadLow(fifo.CPReadPointer);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteLow(fifo.CPReadPointer, val & 0xFFE0);
                 }));
  mmio->Register(base | FIFO_READ_POINTER_HI, MMIO::ComplexRead<u16>([](u32) {
                   SyncForRegisterAccess();
                   return ReadHigh(fifo.CPReadPointer);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   SyncForRegisterAccess();
                   WriteHigh(fifo.CPReadPointer, val);
                 }));

  // Write pointer is updated at GP burst time, so no need to synchronize.
  mmio->Register(base | FIFO_WRITE_POINTER_LO,
                 MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPWritePointer)),
                 MMIO::DirectWrite<u16>(MMIO::Utils::LowPart(&fifo.CPWritePointer)));
  mmio->Register(base | FIFO_WRITE_POINTER_HI,
                 MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPWritePointer)),
                 MMIO::DirectWrite<u16>(MMIO::Utils::HighPart(&fifo.CPWritePointer)));
}

void GatherPipeBursted()
{
  // if we aren't linked, we don't care about gather pipe data
  if (!m_CPCtrlReg.GPLinkEnable)
    return;

  // update the fifo pointer
  if (fifo.CPWritePointer == fifo.CPEnd)
    fifo.CPWritePointer = fifo.CPBase;
  else
    fifo.CPWritePointer += GATHER_PIPE_SIZE;

  fifo.CPReadWriteDistance += GATHER_PIPE_SIZE;

  if (m_CPCtrlReg.GPReadEnable && m_CPCtrlReg.GPLinkEnable)
  {
    ProcessorInterface::Fifo_CPUWritePointer = fifo.CPWritePointer;
    ProcessorInterface::Fifo_CPUBase = fifo.CPBase;
    ProcessorInterface::Fifo_CPUEnd = fifo.CPEnd;
  }

  // Update interrupts. This way we trigger high watermark interrupts as soon as possible.
  UpdateInterrupts();

  // The FIFO can be overflowed by the gatherpipe, if a large number of bytes is written in a single
  // JIT block. In this case, "borrow" some cycles to execute the GPU now instead of later, reducing
  // the amount of data in the FIFO. In dual core, we take this even further, kicking the GPU as
  // soon as there is half a kilobyte of commands. of commands, before kicking the GPU. Kicking on
  // every GP burst is too slow, and waiting too long introduces latency when we eventually do need
  // to synchronize with the GPU thread.
  if (CanReadFromFifo())
  {
    if (SConfig::GetInstance().bCPUThread)
    {
      if (fifo.CPReadWriteDistance >= FIFO_EXECUTE_THRESHOLD_SIZE)
      {
        RunGpuDualCore();
        UpdateSyncEvent();
      }
    }
    else
    {
      const u32 threshold = std::max(u32(GATHER_PIPE_SIZE), (fifo.CPEnd - fifo.CPBase / 2));
      if (fifo.CPReadWriteDistance >= threshold)
        RunGpuSingleCore();

      // Even if we don't have sufficient work now, make sure the GPU is awake (syncing enabled).
      UpdateSyncEvent();
    }
  }

  ASSERT_MSG(COMMANDPROCESSOR, fifo.CPReadWriteDistance <= fifo.CPEnd - fifo.CPBase,
             "FIFO is overflowed by GatherPipe !\nCPU thread is too fast!");

  // check if we are in sync
  ASSERT_MSG(COMMANDPROCESSOR, fifo.CPWritePointer == ProcessorInterface::Fifo_CPUWritePointer,
             "FIFOs linked but out of sync");
  ASSERT_MSG(COMMANDPROCESSOR, fifo.CPBase == ProcessorInterface::Fifo_CPUBase,
             "FIFOs linked but out of sync");
  ASSERT_MSG(COMMANDPROCESSOR, fifo.CPEnd == ProcessorInterface::Fifo_CPUEnd,
             "FIFOs linked but out of sync");
}

bool AtBreakpoint()
{
  return fifo.bFF_BPEnable && (fifo.CPReadPointer == fifo.CPBreakpoint);
}

bool CanReadFromFifo()
{
  return fifo.bFF_GPReadEnable && fifo.CPReadWriteDistance >= GATHER_PIPE_SIZE && !AtBreakpoint();
}

bool GpuIsIdle()
{
  // TODO: Not strictly true, since we can be idle while waiting for the remainder of a command.
  // However, this is only used for SC event scheduling, so it's fine for now.
  return !CanReadFromFifo() && s_video_buffer_size.load() == 0;
}

void UpdateInterrupts()
{
  // breakpoint
  if (fifo.bFF_BPEnable)
  {
    if (fifo.CPBreakpoint == fifo.CPReadPointer)
    {
      if (!fifo.bFF_Breakpoint)
      {
        DEBUG_LOG(COMMANDPROCESSOR, "Hit breakpoint at %i", fifo.CPReadPointer);
        fifo.bFF_Breakpoint = true;
      }
    }
    else
    {
      if (fifo.bFF_Breakpoint)
        DEBUG_LOG(COMMANDPROCESSOR, "Cleared breakpoint at %i", fifo.CPReadPointer);
      fifo.bFF_Breakpoint = false;
    }
  }
  else
  {
    if (fifo.bFF_Breakpoint)
      DEBUG_LOG(COMMANDPROCESSOR, "Cleared breakpoint at %i", fifo.CPReadPointer);
    fifo.bFF_Breakpoint = false;
  }

  // overflow & underflow check
  fifo.bFF_HiWatermark = (fifo.CPReadWriteDistance > fifo.CPHiWatermark);
  fifo.bFF_LoWatermark = (fifo.CPReadWriteDistance < fifo.CPLoWatermark);

  const bool has_interrupt =
      fifo.bFF_GPReadEnable &&  // TODO: Is this correct
      ((fifo.bFF_HiWatermark & fifo.bFF_HiWatermarkInt) |
       (fifo.bFF_LoWatermark & fifo.bFF_LoWatermarkInt) | (fifo.bFF_Breakpoint & fifo.bFF_BPInt));

  if (has_interrupt)
  {
    if (s_interrupt_set.TestAndSet())
    {
      DEBUG_LOG(COMMANDPROCESSOR, "Interrupt set");
      ProcessorInterface::SetInterrupt(INT_CAUSE_CP, true);
      CoreTiming::ForceExceptionCheck(0);
    }
  }
  else
  {
    if (s_interrupt_set.TestAndClear())
    {
      DEBUG_LOG(COMMANDPROCESSOR, "Interrupt cleared");
      ProcessorInterface::SetInterrupt(INT_CAUSE_CP, false);
    }
  }
}

void RunGpuSingleCore()
{
  // Single core - run as many ticks as we have available, stall once we run out.
  s32 available_ticks = s_sync_ticks.load();
  if (available_ticks <= 0 && !CanReadFromFifo())
  {
    // We can't read from the FIFO, or execute any commands.
    // None of the statements below will execute.
    UpdateInterrupts();
    return;
  }

  FPURoundMode::SaveSIMDState();
  FPURoundMode::LoadDefaultSIMDState();

  u32 total_bytes_used = 0;
  s32 total_cycles_used = 0;
  while (total_cycles_used < available_ticks)
  {
    // Read as much as possible from the FIFO.
    // We could read in smaller batches, to make the RW pointer more accurate to the actual
    // GPU progress, but since our timings are garbage anyway, it's unlikely to make much of
    // a difference. Larger batches means fewer loops, and lower CPU usage.
    // CopyToVideoBuffer(FIFO_EXECUTE_THRESHOLD_SIZE);
    CopyToVideoBuffer(FIFO_SIZE);

    // Execute as many commands as possible.
    u32 cyclesExecuted = 0;
    u8* old_read_ptr = s_video_buffer_read_ptr;
    s_video_buffer_read_ptr = OpcodeDecoder::Run(
        DataReader(s_video_buffer_read_ptr, s_video_buffer_write_ptr), &cyclesExecuted, false);

    u32 bytes_consumed = u32(s_video_buffer_read_ptr - old_read_ptr);
    if (bytes_consumed == 0)
    {
      // Waiting for more data.
      break;
    }

    total_bytes_used += bytes_consumed;
    total_cycles_used += static_cast<s32>(cyclesExecuted);

#if 0
    WARN_LOG(VIDEO, "used %u/%u/%u bytes, %d/%d cycles", total_bytes_used,
             s_video_buffer_size.load(), fifo.CPReadWriteDistance, total_cycles_used,
             s_sync_ticks.load());
#endif
  }

#if 0
  if (total_cycles_used >= available_ticks)
    WARN_LOG(VIDEO, "out of ticks %u/%u", total_cycles_used, available_ticks);
#endif

  s_video_buffer_size.fetch_sub(total_bytes_used);
  s_sync_ticks.fetch_sub(total_cycles_used);

  FPURoundMode::LoadSIMDState();
}

void RunGpuDualCore()
{
  // Dual core - simulate an "infinitely fast" GPU, copying as much as possible to the video buffer,
  // and executing it on the GPU thread.
  if (!CanReadFromFifo() && s_video_buffer_size.load() == 0)
  {
    UpdateInterrupts();
    return;
  }

  while (CanReadFromFifo())
    CopyToVideoBuffer(FIFO_SIZE);

  // Wake the GPU thread, as there is new work.
  // We defer the wake until the next SyncGPU if it is enabled, and there are no ticks. Otherwise,
  // the GPU thread would wake, immediately sleep, and then wake again once ticks were added.
  if (!SConfig::GetInstance().bSyncGPU ||
      s_sync_ticks.load() > SConfig::GetInstance().iSyncGpuMinDistance)
  {
    s_gpu_mainloop.Wakeup();
  }
}

void SyncForRegisterAccess()
{
  // Add GPU_TIME_SLOT_SIZE cycles to the GPU, run it, then remove the cycles.
  // This way, the GPU always makes progress, but it will slow down in the next slice to compensate.
  s_sync_ticks.fetch_add(GPU_TIME_SLOT_SIZE);
  if (SConfig::GetInstance().bCPUThread)
    RunGpuDualCore();
  else
    RunGpuSingleCore();
  s_sync_ticks.fetch_sub(GPU_TIME_SLOT_SIZE);
  UpdateSyncEvent();
}

// Description: RunGpuLoop() sends data through this function.
void CopyToVideoBuffer(u32 maximum_copy_size)
{
  UpdateInterrupts();
  while (CanReadFromFifo() && maximum_copy_size > 0)
  {
    // Work out the copy size. We can copy up until the next breakpoint, or the FIFO wraps around.
    u32 copy_size;
    if (fifo.CPReadPointer < fifo.CPEnd)
    {
      copy_size = std::min(maximum_copy_size,
                           std::min(fifo.CPReadWriteDistance, fifo.CPEnd - fifo.CPReadPointer));
      if (fifo.bFF_BPEnable && fifo.CPReadPointer < fifo.CPBreakpoint)
        copy_size = std::min(copy_size, fifo.CPBreakpoint - fifo.CPReadPointer);

      ASSERT(fifo.CPReadWriteDistance >= copy_size);
    }
    else
    {
      // libogc says "Due to the mechanics of flushing the write-gather pipe, the FIFO memory area
      // should be at least 32 bytes larger than the maximum expected amount of data stored". Hence
      // why we do this check after the read. Also see GPFifo.cpp.
      copy_size = GATHER_PIPE_SIZE;
    }

    // Ensure we have space in the video buffer.
    if (copy_size > static_cast<u32>(s_video_buffer + FIFO_SIZE - s_video_buffer_write_ptr))
    {
      std::lock_guard<std::mutex> guard(s_video_buffer_lock);
      size_t existing_len = s_video_buffer_write_ptr - s_video_buffer_read_ptr;
      if (copy_size > (FIFO_SIZE - existing_len))
      {
        PanicAlert("FIFO out of bounds (existing %zu + new %u > %u)", existing_len, copy_size,
                   FIFO_SIZE);
        return;
      }
      std::memmove(s_video_buffer, s_video_buffer_read_ptr, existing_len);
      s_video_buffer_write_ptr = s_video_buffer + existing_len;
      s_video_buffer_read_ptr = s_video_buffer;
    }

    // Copy new video instructions to s_video_buffer for future use in rendering the new picture
    Memory::CopyFromEmu(s_video_buffer_write_ptr, fifo.CPReadPointer, copy_size);
    s_video_buffer_write_ptr += copy_size;
    s_video_buffer_size.fetch_add(copy_size);

    // Update FIFO read pointer.
    if (fifo.CPReadPointer == fifo.CPEnd)
    {
      // See comment above about writing past the end of the FIFO.
      fifo.CPReadPointer = fifo.CPBase;
    }
    else
    {
      fifo.CPReadPointer += copy_size;
    }

    fifo.CPReadWriteDistance -= copy_size;
    maximum_copy_size -= copy_size;
    UpdateInterrupts();
  }
}

static void ExecuteGpuSliceDualCore()
{
  const SConfig& param = SConfig::GetInstance();
  bool did_any_work = false;

  while (s_video_buffer_size.load() > 0)
  {
    // Only use ticks if SyncGPU is enabled.
    if (param.bSyncGPU && s_sync_ticks.load() < param.iSyncGpuMinDistance)
      break;

    std::lock_guard<std::mutex> guard(s_video_buffer_lock);

    // Execute as many commands as possible.
    u32 cyclesExecuted = 0;
    u8* old_read_ptr = s_video_buffer_read_ptr;
    s_video_buffer_read_ptr = OpcodeDecoder::Run(
        DataReader(s_video_buffer_read_ptr, s_video_buffer_write_ptr), &cyclesExecuted, false);
    u32 bytes_consumed = u32(s_video_buffer_read_ptr - old_read_ptr);

#if 0
    WARN_LOG(VIDEO, "used %u/%u/%u bytes, %d/%d cycles", bytes_consumed, s_video_buffer_size.load(),
             fifo.CPReadWriteDistance, cyclesExecuted, s_sync_ticks.load());
#endif

    if (bytes_consumed == 0)
      break;

    if (param.bSyncGPU)
    {
      cyclesExecuted = static_cast<u32>(cyclesExecuted / param.fSyncGpuOverclock);
      s_sync_ticks.fetch_sub(static_cast<s32>(cyclesExecuted));
    }

    s_sync_ticks.fetch_sub(s32(cyclesExecuted));
    s_video_buffer_size.fetch_sub(bytes_consumed);
    did_any_work = true;
  }

  // We likely won't have any work for a while, so ensure the pipeline is flushed.
  if (did_any_work)
    g_vertex_manager->Flush();
  else
    s_gpu_mainloop.AllowSleep();

  AsyncRequests::GetInstance()->PullEvents();
}

// Description: Main FIFO update loop
// Purpose: Keep the Core HW updated about the CPU-GPU distance
void RunGpuLoop()
{
  AsyncRequests::GetInstance()->SetEnable(true);
  AsyncRequests::GetInstance()->SetPassthrough(false);

  s_gpu_mainloop.Run(ExecuteGpuSliceDualCore, 0);

  AsyncRequests::GetInstance()->SetEnable(false);
  AsyncRequests::GetInstance()->SetPassthrough(true);
}

void Flush(bool idle)
{
  // If we're idling, set a maximum number of ticks, so that we execute all commands.
  if (idle)
    s_sync_ticks.store(std::numeric_limits<s32>::max());

  // Run the GPU until it has no more work to do.
  if (SConfig::GetInstance().bCPUThread)
  {
    // In dual-core, when we're idling, ensure the GPU thread has finished all commands.
    RunGpuDualCore();
    if (idle)
      s_gpu_mainloop.Wait();
  }
  else
  {
    RunGpuSingleCore();
  }

  // Clear the sync ticks, it'll get boosted next event.
  if (idle)
    s_sync_ticks.store(0);

  UpdateSyncEvent();
}

void SetCpStatusRegister()
{
  // Here always there is one fifo attached to the GPU
  m_CPStatusReg.Breakpoint = fifo.bFF_Breakpoint;
  m_CPStatusReg.ReadIdle = !fifo.CPReadWriteDistance || (fifo.CPReadPointer == fifo.CPWritePointer);
  m_CPStatusReg.CommandIdle = !fifo.CPReadWriteDistance || AtBreakpoint() || !fifo.bFF_GPReadEnable;
  m_CPStatusReg.UnderflowLoWatermark = fifo.bFF_LoWatermark;
  m_CPStatusReg.OverflowHiWatermark = fifo.bFF_HiWatermark;

  DEBUG_LOG(COMMANDPROCESSOR, "\t Read from STATUS_REGISTER : %04x", m_CPStatusReg.Hex);
  DEBUG_LOG(
      COMMANDPROCESSOR, "(r) status: iBP %s | fReadIdle %s | fCmdIdle %s | iOvF %s | iUndF %s",
      m_CPStatusReg.Breakpoint ? "ON" : "OFF", m_CPStatusReg.ReadIdle ? "ON" : "OFF",
      m_CPStatusReg.CommandIdle ? "ON" : "OFF", m_CPStatusReg.OverflowHiWatermark ? "ON" : "OFF",
      m_CPStatusReg.UnderflowLoWatermark ? "ON" : "OFF");
}

void SetCpControlRegister()
{
  fifo.bFF_BPInt = m_CPCtrlReg.BPInt != 0;
  fifo.bFF_BPEnable = m_CPCtrlReg.BPEnable != 0;
  fifo.bFF_HiWatermarkInt = m_CPCtrlReg.FifoOverflowIntEnable != 0;
  fifo.bFF_LoWatermarkInt = m_CPCtrlReg.FifoUnderflowIntEnable != 0;
  fifo.bFF_GPLinkEnable = m_CPCtrlReg.GPLinkEnable != 0;
  fifo.bFF_GPReadEnable = m_CPCtrlReg.GPReadEnable != 0;

  DEBUG_LOG(COMMANDPROCESSOR, "\t GPREAD %s | BP %s | Int %s | OvF %s | UndF %s | LINK %s",
            fifo.bFF_GPReadEnable ? "ON" : "OFF", fifo.bFF_BPEnable ? "ON" : "OFF",
            fifo.bFF_BPInt ? "ON" : "OFF", m_CPCtrlReg.FifoOverflowIntEnable ? "ON" : "OFF",
            m_CPCtrlReg.FifoUnderflowIntEnable ? "ON" : "OFF",
            m_CPCtrlReg.GPLinkEnable ? "ON" : "OFF");
}

// NOTE: We intentionally don't emulate this function at the moment.
// We don't emulate proper GP timing anyway at the moment, so it would just slow down emulation.
void SetCpClearRegister()
{
}

void GpuMaySleep()
{
  if (SConfig::GetInstance().bCPUThread)
    s_gpu_mainloop.AllowSleep();
}

void WakeGpu()
{
  // This function is called when a control register changes, or the FIFO threshold is exceeded.
  if (SConfig::GetInstance().bCPUThread)
  {
    // For dual-core, we have to read from the FIFO on the CPU thread, and kick the GPU thread.
    // Otherwise, if a game triggers work by poking the FIFO registers, it will never get read.
    RunGpuDualCore();
  }

  // if the sync GPU callback is suspended, wake it up.
  UpdateSyncEvent();
}

/* This function checks the emulated CPU - GPU distance and may wake up the GPU,
 * or block the CPU if required. It should be called by the CPU thread regularly.
 * @ticks The gone emulated CPU time.
 * @return A good time to call WaitForGpuThread() next.
 */
static int WaitForGpuThread(int ticks)
{
  const SConfig& param = SConfig::GetInstance();

  RunGpuDualCore();

  int old = s_sync_ticks.fetch_add(ticks);
  int now = old + ticks;

  // GPU is idle, so stop polling.
  if (old >= 0 && GpuIsIdle() && s_gpu_mainloop.IsDone())
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
  int prev_extra_ticks = s_sync_ticks.load();
  int ticks_to_add =
      static_cast<int>((ticks + cyclesLate) * SConfig::GetInstance().fSyncGpuOverclock) -
      prev_extra_ticks;
  int next = -1;

  if (!SConfig::GetInstance().bCPUThread)
  {
    s_sync_ticks.fetch_add(ticks_to_add);
    RunGpuSingleCore();

    // Cancel the event if the GPU is now idle.
    next = GpuIsIdle() ? -1 : (-s_sync_ticks.load() + GPU_TIME_SLOT_SIZE);
  }
  else if (SConfig::GetInstance().bSyncGPU)
  {
    next = WaitForGpuThread(ticks);
  }

  s_syncing_suspended = next < 0;
  if (!s_syncing_suspended)
    CoreTiming::ScheduleEvent(next, s_event_sync_gpu, next);
#if 0
  else
    WARN_LOG(VIDEO, "Syncing disabled");
#endif
}

// Initialize GPU - CPU thread syncing, this gives us a deterministic way to start the GPU thread.
void Prepare()
{
  s_event_sync_gpu = CoreTiming::RegisterEvent("SyncGPUCallback", SyncGPUCallback);
  s_syncing_suspended = true;
  UpdateSyncEvent();
}

void UpdateSyncEvent()
{
  if (SConfig::GetInstance().bCPUThread && !SConfig::GetInstance().bSyncGPU)
    return;

  bool syncing_suspended;
  if (!SConfig::GetInstance().bCPUThread)
  {
    // For single-core, we need the event if there is a non-empty FIFO, or a non-empty video buffer.
    syncing_suspended = GpuIsIdle();
  }
  else
  {
    // For SyncGPU, the GPU thread must be busy.
    syncing_suspended = GpuIsIdle() && s_gpu_mainloop.IsDone();
  }

  if (syncing_suspended != s_syncing_suspended)
  {
#if 0
    WARN_LOG(VIDEO, "syncing %s", syncing_suspended ? "disabled" : "enabled");
#endif
    if (syncing_suspended)
      CoreTiming::RemoveEvent(s_event_sync_gpu);
    else
      CoreTiming::ScheduleEvent(GPU_TIME_SLOT_SIZE, s_event_sync_gpu, GPU_TIME_SLOT_SIZE);

    s_syncing_suspended = syncing_suspended;
  }
}

void HandleUnknownOpcode(u8 cmd_byte, void* buffer)
{
  // TODO(Omega): Maybe dump FIFO to file on this error
  PanicAlertT("GFX FIFO: Unknown Opcode (0x%02x @ %p).\n"
              "This means one of the following:\n"
              "* The emulated GPU got desynced, disabling dual core can help\n"
              "* Command stream corrupted by some spurious memory bug\n"
              "* This really is an unknown opcode (unlikely)\n"
              "* Some other sort of bug\n\n"
              "Further errors will be sent to the Video Backend log and\n"
              "Dolphin will now likely crash or hang. Enjoy.",
              cmd_byte, buffer);

  {
    PanicAlert("Illegal command %02x\n"
               "CPBase: 0x%08x\n"
               "CPEnd: 0x%08x\n"
               "CPHiWatermark: 0x%08x\n"
               "CPLoWatermark: 0x%08x\n"
               "CPReadWriteDistance: 0x%08x\n"
               "CPWritePointer: 0x%08x\n"
               "CPReadPointer: 0x%08x\n"
               "CPBreakpoint: 0x%08x\n"
               "bFF_GPReadEnable: %s\n"
               "bFF_BPEnable: %s\n"
               "bFF_BPInt: %s\n"
               "bFF_Breakpoint: %s\n"
               "bFF_GPLinkEnable: %s\n"
               "bFF_HiWatermarkInt: %s\n"
               "bFF_LoWatermarkInt: %s\n",
               cmd_byte, fifo.CPBase, fifo.CPEnd, fifo.CPHiWatermark, fifo.CPLoWatermark,
               fifo.CPReadWriteDistance, fifo.CPWritePointer, fifo.CPReadPointer, fifo.CPBreakpoint,
               fifo.bFF_GPReadEnable ? "true" : "false", fifo.bFF_BPEnable ? "true" : "false",
               fifo.bFF_BPInt ? "true" : "false", fifo.bFF_Breakpoint ? "true" : "false",
               fifo.bFF_GPLinkEnable ? "true" : "false", fifo.bFF_HiWatermarkInt ? "true" : "false",
               fifo.bFF_LoWatermarkInt ? "true" : "false");
  }
}

}  // namespace Fifo
