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
static constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;
static constexpr int GPU_TIME_SLOT_SIZE = 1000;

static Common::BlockingLoop s_gpu_mainloop;

static Common::Flag s_emu_running_state;
static CoreTiming::EventType* s_event_sync_gpu;

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

static bool RunGpu(bool needs_simd_reset);

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

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  struct
  {
    u32 addr;
    u16* ptr;
    bool readonly;
    bool writes_align_to_32_bytes;
  } directly_mapped_vars[] = {
      {FIFO_TOKEN_REGISTER, &m_tokenReg},

      // Bounding box registers are read only.
      {FIFO_BOUNDING_BOX_LEFT, &m_bboxleft, true},
      {FIFO_BOUNDING_BOX_RIGHT, &m_bboxright, true},
      {FIFO_BOUNDING_BOX_TOP, &m_bboxtop, true},
      {FIFO_BOUNDING_BOX_BOTTOM, &m_bboxbottom, true},

      // Some FIFO addresses need to be aligned on 32 bytes on write - only
      // the high part can be written directly without a mask.
      {FIFO_BASE_LO, MMIO::Utils::LowPart(&fifo.CPBase), false, true},
      {FIFO_BASE_HI, MMIO::Utils::HighPart(&fifo.CPBase)},
      {FIFO_END_LO, MMIO::Utils::LowPart(&fifo.CPEnd), false, true},
      {FIFO_END_HI, MMIO::Utils::HighPart(&fifo.CPEnd)},
      {FIFO_HI_WATERMARK_LO, MMIO::Utils::LowPart(&fifo.CPHiWatermark)},
      {FIFO_HI_WATERMARK_HI, MMIO::Utils::HighPart(&fifo.CPHiWatermark)},
      {FIFO_LO_WATERMARK_LO, MMIO::Utils::LowPart(&fifo.CPLoWatermark)},
      {FIFO_LO_WATERMARK_HI, MMIO::Utils::HighPart(&fifo.CPLoWatermark)},
      // FIFO_RW_DISTANCE has some complex read code different for
      // single/dual core.
      {FIFO_WRITE_POINTER_LO, MMIO::Utils::LowPart(&fifo.CPWritePointer), false, true},
      {FIFO_WRITE_POINTER_HI, MMIO::Utils::HighPart(&fifo.CPWritePointer)},
      // FIFO_READ_POINTER has different code for single/dual core.
  };

  for (auto& mapped_var : directly_mapped_vars)
  {
    u16 wmask = mapped_var.writes_align_to_32_bytes ? 0xFFE0 : 0xFFFF;
    mmio->Register(base | mapped_var.addr, MMIO::DirectRead<u16>(mapped_var.ptr),
                   mapped_var.readonly ? MMIO::InvalidWrite<u16>() :
                                         MMIO::DirectWrite<u16>(mapped_var.ptr, wmask));
  }

  mmio->Register(
      base | FIFO_BP_LO, MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPBreakpoint)),
      MMIO::ComplexWrite<u16>([](u32, u16 val) { WriteLow(fifo.CPBreakpoint, val & 0xffe0); }));
  mmio->Register(base | FIFO_BP_HI,
                 MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPBreakpoint)),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) { WriteHigh(fifo.CPBreakpoint, val); }));

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

  mmio->Register(base | STATUS_REGISTER, MMIO::ComplexRead<u16>([](u32) {
                   Run();
                   SetCpStatusRegister();
                   return m_CPStatusReg.Hex;
                 }),
                 MMIO::InvalidWrite<u16>());

  mmio->Register(base | CTRL_REGISTER, MMIO::DirectRead<u16>(&m_CPCtrlReg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Run();
                   UCPCtrlReg tmp(val);
                   m_CPCtrlReg.Hex = tmp.Hex;
                   SetCpControlRegister();
                   Run();
                 }));

  mmio->Register(base | CLEAR_REGISTER, MMIO::DirectRead<u16>(&m_CPClearReg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   UCPClearReg tmp(val);
                   m_CPClearReg.Hex = tmp.Hex;
                   SetCpClearRegister();
                 }));

  mmio->Register(base | PERF_SELECT, MMIO::InvalidRead<u16>(), MMIO::Nop<u16>());

  // Some MMIOs have different handlers for single core vs. dual core mode.
  mmio->Register(base | FIFO_RW_DISTANCE_LO, MMIO::ComplexRead<u16>([](u32) {
                   Run();
                   return ReadLow(fifo.CPReadWriteDistance);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Run();
                   WriteLow(fifo.CPReadWriteDistance, val & 0xFFE0);
                 }));
  mmio->Register(base | FIFO_RW_DISTANCE_HI, MMIO::ComplexRead<u16>([](u32) {
                   Run();
                   return ReadHigh(fifo.CPReadWriteDistance);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Run();
                   WriteHigh(fifo.CPReadWriteDistance, val);

                   // TODO: Is this correct?
                   if (fifo.CPReadWriteDistance == 0)
                     GPFifo::ResetGatherPipe();

                   Run();
                 }));
  mmio->Register(base | FIFO_READ_POINTER_LO, MMIO::ComplexRead<u16>([](u32) {
                   Run();
                   return ReadLow(fifo.CPReadPointer);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Run();
                   WriteLow(fifo.CPReadPointer, val & 0xFFE0);
                 }));
  mmio->Register(base | FIFO_READ_POINTER_HI, MMIO::ComplexRead<u16>([](u32) {
                   Run();
                   return ReadHigh(fifo.CPReadPointer);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Run();
                   WriteHigh(fifo.CPReadPointer, val);
                 }));
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

  // Only run once we are half the buffer behind.
  // This should be faster, as we're processing larger batches at once.
  // For dual core mode, using a larger buffer here makes things slower, since there is
  // more of a delay to synchronize with the GPU thread. So wake it more often.
  const u32 run_threshold =
      SConfig::GetInstance().bCPUThread ? 512 : (fifo.CPEnd - fifo.CPBase) / 2;
  if (fifo.CPReadWriteDistance >= run_threshold)
    Run();

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

static bool AtBreakpoint()
{
  return fifo.bFF_BPEnable && (fifo.CPReadPointer == fifo.CPBreakpoint);
}

static bool CanRun()
{
  return fifo.bFF_GPReadEnable && fifo.CPReadWriteDistance >= GATHER_PIPE_SIZE && !AtBreakpoint();
}

void Run()
{
  // TODO: Hi watermark interrupts without breakpoints are currently broken. The interrupt will be
  // set, but then immediately cleared.
  UpdateInterrupts();
  if (!CanRun())
    return;
  do
  {
    // Work out the copy size. We can copy up until the next breakpoint, or the FIFO wraps around.
    u32 copy_size;
    if (fifo.CPReadPointer != fifo.CPEnd)
    {
      copy_size = std::min(fifo.CPReadWriteDistance, fifo.CPEnd - fifo.CPReadPointer);
      if (fifo.bFF_BPEnable && fifo.CPReadPointer < fifo.CPBreakpoint)
        copy_size = std::min(copy_size, fifo.CPBreakpoint - fifo.CPReadPointer);
      copy_size = std::min(copy_size, FIFO_SIZE);
    }
    else
    {
      // libogc says "Due to the mechanics of flushing the write-gather pipe, the FIFO memory area
      // should be at least 32 bytes larger than the maximum expected amount of data stored". Hence
      // why we do this check after the read. Also see GPFifo.cpp.
      copy_size = GATHER_PIPE_SIZE;
    }

    // Ensure copy_size is aligned to 32 bytes. It should be...
    ASSERT((copy_size % 32) == 0);
    Fifo::ReadDataFromFifo(fifo.CPReadPointer, copy_size);

    if (fifo.CPReadPointer == fifo.CPEnd)
      fifo.CPReadPointer = fifo.CPBase;
    else
      fifo.CPReadPointer += copy_size;

    fifo.CPReadWriteDistance -= copy_size;

    UpdateInterrupts();
  } while (CanRun());

  Fifo::WakeGpu();
}

void Flush(bool idle)
{
  Run();
  if (idle)
    Fifo::WaitForGpu(true);
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
  WaitForGpu(true);

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

void WaitForGpu(bool idle)
{
  u32 buffer_size = s_video_buffer_size.load();
  if (buffer_size == 0)
    return;

  if (idle)
  {
    // For idle skip, we want to ensure the GPU is completely finished.
    // The distance is always greater than the cycles, so this'll do.
    s_sync_ticks.fetch_add(buffer_size);
  }

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
