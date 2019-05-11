// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <atomic>
#include <cstring>

#include "Common/Assert.h"
#include "Common/Atomic.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"

namespace CommandProcessor
{
static CoreTiming::EventType* et_UpdateInterrupts;

// TODO(ector): Warn on bbox read/write

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
static Common::Flag s_interrupt_waiting;

static bool IsOnThread()
{
  return SConfig::GetInstance().bCPUThread;
}

static void UpdateInterrupts_Wrapper(u64 userdata, s64 cyclesLate)
{
  UpdateInterrupts(userdata);
}

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
  p.Do(SafeCPReadPointer);

  p.Do(GPLinkEnable);
  p.Do(ReadEnable);
  p.Do(BreakpointEnable);
  p.Do(BreakpointInterruptEnable);
  p.Do(BreakpointFlag);

  p.Do(UnderflowInterruptEnable);
  p.Do(OverflowInterruptEnable);

  p.Do(UnderflowFlag);
  p.Do(OverflowFlag);
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
  p.Do(s_interrupt_waiting);
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
  fifo.BreakpointFlag = 0;
  fifo.OverflowFlag = 0;
  fifo.OverflowInterruptEnable = 0;
  fifo.UnderflowFlag = 0;
  fifo.UnderflowInterruptEnable = 0;

  s_interrupt_set.Clear();
  s_interrupt_waiting.Clear();

  et_UpdateInterrupts = CoreTiming::RegisterEvent("CPInterrupt", UpdateInterrupts_Wrapper);
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  constexpr u16 WMASK_NONE = 0x0000;
  constexpr u16 WMASK_ALL = 0xffff;
  constexpr u16 WMASK_LO_ALIGN_32BIT = 0xffe0;
  const u16 WMASK_HI_RESTRICT = SConfig::GetInstance().bWii ? 0x1fff : 0x03ff;

  struct
  {
    u32 addr;
    u16* ptr;
    bool readonly;
    // FIFO mmio regs in the range [cc000020-cc00003e] have certain bits that always read as 0
    // For _LO registers in this range, only bits 0xffe0 can be set
    // For _HI registers in this range, only bits 0x03ff can be set on GCN and 0x1fff on Wii
    u16 wmask;
  } directly_mapped_vars[] = {
      {FIFO_TOKEN_REGISTER, &m_tokenReg, false, WMASK_ALL},

      // Bounding box registers are read only.
      {FIFO_BOUNDING_BOX_LEFT, &m_bboxleft, true, WMASK_NONE},
      {FIFO_BOUNDING_BOX_RIGHT, &m_bboxright, true, WMASK_NONE},
      {FIFO_BOUNDING_BOX_TOP, &m_bboxtop, true, WMASK_NONE},
      {FIFO_BOUNDING_BOX_BOTTOM, &m_bboxbottom, true, WMASK_NONE},
      {FIFO_BASE_LO, MMIO::Utils::LowPart(&fifo.CPBase), false, WMASK_LO_ALIGN_32BIT},
      {FIFO_BASE_HI, MMIO::Utils::HighPart(&fifo.CPBase), false, WMASK_HI_RESTRICT},
      {FIFO_END_LO, MMIO::Utils::LowPart(&fifo.CPEnd), false, WMASK_LO_ALIGN_32BIT},
      {FIFO_END_HI, MMIO::Utils::HighPart(&fifo.CPEnd), false, WMASK_HI_RESTRICT},
      {FIFO_HI_WATERMARK_LO, MMIO::Utils::LowPart(&fifo.CPHiWatermark), false,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_HI_WATERMARK_HI, MMIO::Utils::HighPart(&fifo.CPHiWatermark), false, WMASK_HI_RESTRICT},
      {FIFO_LO_WATERMARK_LO, MMIO::Utils::LowPart(&fifo.CPLoWatermark), false,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_LO_WATERMARK_HI, MMIO::Utils::HighPart(&fifo.CPLoWatermark), false, WMASK_HI_RESTRICT},
      // FIFO_RW_DISTANCE has some complex read code different for
      // single/dual core.
      {FIFO_WRITE_POINTER_LO, MMIO::Utils::LowPart(&fifo.CPWritePointer), false,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_WRITE_POINTER_HI, MMIO::Utils::HighPart(&fifo.CPWritePointer), false,
       WMASK_HI_RESTRICT},
      // FIFO_READ_POINTER has different code for single/dual core.
  };

  for (auto& mapped_var : directly_mapped_vars)
  {
    mmio->Register(base | mapped_var.addr, MMIO::DirectRead<u16>(mapped_var.ptr),
                   mapped_var.readonly ? MMIO::InvalidWrite<u16>() :
                                         MMIO::DirectWrite<u16>(mapped_var.ptr, mapped_var.wmask));
  }

  mmio->Register(base | FIFO_BP_LO, MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPBreakpoint)),
                 MMIO::ComplexWrite<u16>([WMASK_LO_ALIGN_32BIT](u32, u16 val) {
                   WriteLow(fifo.CPBreakpoint, val & WMASK_LO_ALIGN_32BIT);
                 }));
  mmio->Register(base | FIFO_BP_HI,
                 MMIO::DirectRead<u16>(MMIO::Utils::HighPart(&fifo.CPBreakpoint)),
                 MMIO::ComplexWrite<u16>([WMASK_HI_RESTRICT](u32, u16 val) {
                   WriteHigh(fifo.CPBreakpoint, val & WMASK_HI_RESTRICT);
                 }));

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
                   Fifo::SyncGPUForRegisterAccess(false);
                   SetCpStatusRegister();
                   return m_CPStatusReg.Hex;
                 }),
                 MMIO::InvalidWrite<u16>());

  mmio->Register(base | CTRL_REGISTER, MMIO::DirectRead<u16>(&m_CPCtrlReg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   UCPCtrlReg tmp(val);
                   m_CPCtrlReg.Hex = tmp.Hex;
                   SetCpControlRegister();
                 }));

  mmio->Register(base | CLEAR_REGISTER, MMIO::DirectRead<u16>(&m_CPClearReg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   UCPClearReg tmp(val);
                   m_CPClearReg.Hex = tmp.Hex;
                   SetCpClearRegister();
                 }));

  mmio->Register(base | PERF_SELECT, MMIO::InvalidRead<u16>(), MMIO::Nop<u16>());

  // Some MMIOs have different handlers for single core vs. dual core mode.
  mmio->Register(base | FIFO_RW_DISTANCE_LO,
                 IsOnThread() ?
                     MMIO::ComplexRead<u16>([](u32) {
                       if (fifo.CPWritePointer >= fifo.SafeCPReadPointer)
                         return ReadLow(fifo.CPWritePointer - fifo.SafeCPReadPointer);
                       else
                         return ReadLow(fifo.CPEnd - fifo.SafeCPReadPointer + fifo.CPWritePointer -
                                        fifo.CPBase + 32);
                     }) :
                     MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPReadWriteDistance)),
                 MMIO::DirectWrite<u16>(MMIO::Utils::LowPart(&fifo.CPReadWriteDistance),
                                        WMASK_LO_ALIGN_32BIT));
  mmio->Register(base | FIFO_RW_DISTANCE_HI,
                 IsOnThread() ? MMIO::ComplexRead<u16>([](u32) {
                   Fifo::SyncGPUForRegisterAccess(false);
                   if (fifo.CPWritePointer >= fifo.SafeCPReadPointer)
                     return ReadHigh(fifo.CPWritePointer - fifo.SafeCPReadPointer);
                   else
                     return ReadHigh(fifo.CPEnd - fifo.SafeCPReadPointer + fifo.CPWritePointer -
                                     fifo.CPBase + 32);
                 }) :
                                MMIO::ComplexRead<u16>([](u32) {
                                  Fifo::SyncGPUForRegisterAccess(false);
                                  return ReadHigh(fifo.CPReadWriteDistance);
                                }),
                 MMIO::ComplexWrite<u16>([WMASK_HI_RESTRICT](u32, u16 val) {
                   Fifo::SyncGPUForRegisterAccess(true);
                   WriteHigh(fifo.CPReadWriteDistance, val & WMASK_HI_RESTRICT);
                   Fifo::RunGpu();
                 }));
  mmio->Register(base | FIFO_READ_POINTER_LO,
                 IsOnThread() ?
                     MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.SafeCPReadPointer)) :
                     MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&fifo.CPReadPointer)),
                 MMIO::DirectWrite<u16>(MMIO::Utils::LowPart(&fifo.CPReadPointer), 0xFFE0));
  mmio->Register(base | FIFO_READ_POINTER_HI,
                 IsOnThread() ? MMIO::ComplexRead<u16>([](u32) {
                   Fifo::SyncGPUForRegisterAccess(false);
                   return ReadHigh(fifo.SafeCPReadPointer);
                 }) :
                                MMIO::ComplexRead<u16>([](u32) {
                                  Fifo::SyncGPUForRegisterAccess(false);
                                  return ReadHigh(fifo.CPReadPointer);
                                }),
                 IsOnThread() ? MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Fifo::SyncGPUForRegisterAccess(true);
                   WriteHigh(fifo.CPReadPointer, val);
                   fifo.SafeCPReadPointer = fifo.CPReadPointer;
                 }) :
                                MMIO::ComplexWrite<u16>([WMASK_HI_RESTRICT](u32, u16 val) {
                                  Fifo::SyncGPUForRegisterAccess(true);
                                  WriteHigh(fifo.CPReadPointer, val & WMASK_HI_RESTRICT);
                                }));
}

bool AtBreakpoint()
{
  return fifo.BreakpointEnable && (fifo.CPReadPointer == fifo.CPBreakpoint);
}

bool CanReadFromFifo()
{
  return fifo.ReadEnable && fifo.CPReadWriteDistance >= GATHER_PIPE_SIZE && !AtBreakpoint();
}

void GatherPipeBursted()
{
  // if we aren't linked, we don't care about gather pipe data
  if (!m_CPCtrlReg.GPLinkEnable)
  {
    if (IsOnThread() && !Fifo::UseDeterministicGPUThread())
    {
      // In multibuffer mode is not allowed write in the same FIFO attached to the GPU.
      // Fix Pokemon XD in DC mode.
      if ((ProcessorInterface::Fifo_CPUEnd == fifo.CPEnd) &&
          (ProcessorInterface::Fifo_CPUBase == fifo.CPBase) && fifo.CPReadWriteDistance > 0)
      {
        Fifo::FlushGpu();
      }
    }
    Fifo::RunGpu();
    return;
  }

  // update the fifo pointer
  if (fifo.CPWritePointer == fifo.CPEnd)
    fifo.CPWritePointer = fifo.CPBase;
  else
    fifo.CPWritePointer += GATHER_PIPE_SIZE;

  if (m_CPCtrlReg.ReadEnable && m_CPCtrlReg.GPLinkEnable)
  {
    ProcessorInterface::Fifo_CPUWritePointer = fifo.CPWritePointer;
    ProcessorInterface::Fifo_CPUBase = fifo.CPBase;
    ProcessorInterface::Fifo_CPUEnd = fifo.CPEnd;
  }

  Common::AtomicAdd(fifo.CPReadWriteDistance, GATHER_PIPE_SIZE);

  // If this write will exceed the high watermark, run the GPU before incrementing the distance.
  // This way, the interrupt only fires when there is a true overflow, and not just because the
  // last GPU sync was a while ago.
  if (fifo.CPReadWriteDistance >= fifo.CPHiWatermark)
  {
    Fifo::SyncGPUForRegisterAccess(true);
  }

  // check for overflows..
  if (fifo.OverflowInterruptEnable && fifo.CPReadWriteDistance >= fifo.CPHiWatermark &&
      !fifo.OverflowFlag)
  {
    fifo.OverflowFlag = true;
  }

  SetCPStatusFromCPU();

  Fifo::RunGpu();

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

void UpdateInterrupts(u64 userdata)
{
  if (userdata)
  {
    s_interrupt_set.Set();
    DEBUG_LOG(COMMANDPROCESSOR, "Interrupt set");
    ProcessorInterface::SetInterrupt(INT_CAUSE_CP, true);
  }
  else
  {
    s_interrupt_set.Clear();
    DEBUG_LOG(COMMANDPROCESSOR, "Interrupt cleared");
    ProcessorInterface::SetInterrupt(INT_CAUSE_CP, false);
  }
  CoreTiming::ForceExceptionCheck(0);
  s_interrupt_waiting.Clear();
  Fifo::RunGpu();
}

void UpdateInterruptsFromVideoBackend(u64 userdata)
{
  if (!Fifo::UseDeterministicGPUThread())
    CoreTiming::ScheduleEvent(0, et_UpdateInterrupts, userdata, CoreTiming::FromThread::NON_CPU);
}

bool IsInterruptWaiting()
{
  return s_interrupt_waiting.IsSet();
}

void SetCPStatusFromGPU()
{
  // breakpoint
  if (fifo.BreakpointEnable)
  {
    if (fifo.CPBreakpoint == fifo.CPReadPointer)
    {
      if (!fifo.BreakpointFlag)
      {
        DEBUG_LOG(COMMANDPROCESSOR, "Hit breakpoint at %i", fifo.CPReadPointer);
        fifo.BreakpointFlag = true;
      }
    }
    else
    {
      if (fifo.BreakpointFlag)
        DEBUG_LOG(COMMANDPROCESSOR, "Cleared breakpoint at %i", fifo.CPReadPointer);
      fifo.BreakpointFlag = false;
    }
  }
  else
  {
    if (fifo.BreakpointFlag)
      DEBUG_LOG(COMMANDPROCESSOR, "Cleared breakpoint at %i", fifo.CPReadPointer);
    fifo.BreakpointFlag = false;
  }

  // overflow & underflow check
  if (fifo.UnderflowInterruptEnable && fifo.CPReadWriteDistance < fifo.CPLoWatermark &&
      !fifo.UnderflowFlag)
  {
    fifo.UnderflowFlag = true;
  }

  bool bpInt = fifo.BreakpointFlag && fifo.BreakpointInterruptEnable;
  bool ovfInt = fifo.OverflowFlag && fifo.OverflowInterruptEnable;
  bool undfInt = fifo.UnderflowFlag && fifo.UnderflowInterruptEnable;
  bool interrupt = bpInt || ovfInt || undfInt;

  if (interrupt != s_interrupt_set.IsSet() && !s_interrupt_waiting.IsSet())
  {
    u64 userdata = interrupt ? 1 : 0;
    if (IsOnThread())
    {
      if (!interrupt || bpInt || undfInt || ovfInt)
      {
        // Schedule the interrupt asynchronously
        s_interrupt_waiting.Set();
        CommandProcessor::UpdateInterruptsFromVideoBackend(userdata);
      }
    }
    else
    {
      CommandProcessor::UpdateInterrupts(userdata);
    }
  }
}

void SetCPStatusFromCPU()
{
  bool bpInt = fifo.BreakpointFlag && fifo.BreakpointInterruptEnable;
  bool ovfInt = fifo.OverflowFlag && fifo.OverflowInterruptEnable;
  bool undfInt = fifo.UnderflowFlag && fifo.UnderflowInterruptEnable;
  bool interrupt = bpInt || ovfInt || undfInt;

  if (interrupt != s_interrupt_set.IsSet() && !s_interrupt_waiting.IsSet())
  {
    u64 userdata = interrupt ? 1 : 0;
    if (IsOnThread())
    {
      if (!interrupt || bpInt || undfInt || ovfInt)
      {
        s_interrupt_set.Set(interrupt);
        DEBUG_LOG(COMMANDPROCESSOR, "Interrupt set");
        ProcessorInterface::SetInterrupt(INT_CAUSE_CP, interrupt);
        CoreTiming::ForceExceptionCheck(0);
      }
    }
    else
    {
      CommandProcessor::UpdateInterrupts(userdata);
    }
  }
}

void SetCpStatusRegister()
{
  // Here always there is one fifo attached to the GPU
  m_CPStatusReg.Breakpoint = fifo.BreakpointFlag;
  m_CPStatusReg.ReadIdle = !CanReadFromFifo() && !AtBreakpoint();
  m_CPStatusReg.CommandIdle = !CanReadFromFifo();
  m_CPStatusReg.Underflow = fifo.UnderflowFlag;
  m_CPStatusReg.Overflow = fifo.OverflowFlag;

  DEBUG_LOG(COMMANDPROCESSOR, "\t Read from STATUS_REGISTER : %04x", m_CPStatusReg.Hex);
  DEBUG_LOG(COMMANDPROCESSOR,
            "(r) status: iBP %s | fReadIdle %s | fCmdIdle %s | iOvF %s | iUndF %s",
            m_CPStatusReg.Breakpoint ? "ON" : "OFF", m_CPStatusReg.ReadIdle ? "ON" : "OFF",
            m_CPStatusReg.CommandIdle ? "ON" : "OFF", m_CPStatusReg.Overflow ? "ON" : "OFF",
            m_CPStatusReg.Underflow ? "ON" : "OFF");
}

void SetCpControlRegister()
{
  // In dual-core mode, set ReadEnable before syncing. This will force the video thread to break out
  // of the main loop, stopping processing any further commands, which in turn reduces the latency
  // when we do sync.
  if (fifo.ReadEnable && !m_CPCtrlReg.ReadEnable)
    fifo.ReadEnable = false;

  // Only force sync when registers read by the GPU thread are modified.
  if (fifo.BreakpointEnable != m_CPCtrlReg.BPEnable ||
      fifo.BreakpointInterruptEnable != m_CPCtrlReg.BPInt ||
      fifo.OverflowInterruptEnable != m_CPCtrlReg.OverflowIntEnable ||
      fifo.UnderflowInterruptEnable != m_CPCtrlReg.UnderflowIntEnable ||
      fifo.ReadEnable != m_CPCtrlReg.ReadEnable)
  {
    Fifo::SyncGPUForRegisterAccess(true);
  }

  fifo.BreakpointEnable = m_CPCtrlReg.BPEnable;
  fifo.BreakpointInterruptEnable = m_CPCtrlReg.BPInt;
  fifo.OverflowInterruptEnable = m_CPCtrlReg.OverflowIntEnable;
  fifo.UnderflowInterruptEnable = m_CPCtrlReg.UnderflowIntEnable;
  fifo.ReadEnable = m_CPCtrlReg.ReadEnable;
  fifo.GPLinkEnable = m_CPCtrlReg.GPLinkEnable;

  if (CanReadFromFifo())
    Fifo::RunGpu();

  DEBUG_LOG(COMMANDPROCESSOR, "\t GPREAD %s | BP %s | Int %s | OvF %s | UndF %s | LINK %s",
            fifo.ReadEnable ? "ON" : "OFF", fifo.BreakpointEnable ? "ON" : "OFF",
            fifo.BreakpointInterruptEnable ? "ON" : "OFF",
            m_CPCtrlReg.OverflowIntEnable ? "ON" : "OFF",
            m_CPCtrlReg.UnderflowIntEnable ? "ON" : "OFF", m_CPCtrlReg.GPLinkEnable ? "ON" : "OFF");
}

void SetCpClearRegister()
{
  if (m_CPClearReg.ClearFifoOverflow || m_CPClearReg.ClearFifoUnderflow)
    Fifo::SyncGPUForRegisterAccess(true);

  if (m_CPClearReg.ClearFifoOverflow != 0)
  {
    DEBUG_LOG(COMMANDPROCESSOR, "Cleared overflow interrupt at %08X %u", fifo.CPReadPointer,
              fifo.CPReadWriteDistance);
    fifo.OverflowFlag = false;
  }
  if (m_CPClearReg.ClearFifoUnderflow != 0)
  {
    DEBUG_LOG(COMMANDPROCESSOR, "CLEARED underflow interrupt at %08X %u", fifo.CPReadPointer,
              fifo.CPReadWriteDistance);
    fifo.UnderflowFlag = false;
  }
}

void HandleUnknownOpcode(u8 cmd_byte, void* buffer, bool preprocess)
{
  // TODO(Omega): Maybe dump FIFO to file on this error
  PanicAlertT("GFX FIFO: Unknown Opcode (0x%02x @ %p, %s).\n"
              "This means one of the following:\n"
              "* The emulated GPU got desynced, disabling dual core can help\n"
              "* Command stream corrupted by some spurious memory bug\n"
              "* This really is an unknown opcode (unlikely)\n"
              "* Some other sort of bug\n\n"
              "Further errors will be sent to the Video Backend log and\n"
              "Dolphin will now likely crash or hang. Enjoy.",
              cmd_byte, buffer, preprocess ? "preprocess=true" : "preprocess=false");

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
               fifo.ReadEnable ? "true" : "false", fifo.BreakpointEnable ? "true" : "false",
               fifo.BreakpointInterruptEnable ? "true" : "false",
               fifo.BreakpointFlag ? "true" : "false", fifo.GPLinkEnable ? "true" : "false",
               fifo.OverflowInterruptEnable ? "true" : "false",
               fifo.UnderflowInterruptEnable ? "true" : "false");
  }
}

}  // end of namespace CommandProcessor
