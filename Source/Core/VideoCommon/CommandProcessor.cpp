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
  fifo.bFF_Breakpoint = 0;
  fifo.bFF_HiWatermark = 0;
  fifo.bFF_HiWatermarkInt = 0;
  fifo.bFF_LoWatermark = 0;
  fifo.bFF_LoWatermarkInt = 0;

  s_interrupt_set.Clear();
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
                   UpdateInterrupts();
                 }));

  mmio->Register(base | CLEAR_REGISTER, MMIO::DirectRead<u16>(&m_CPClearReg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Run();
                   UCPClearReg tmp(val);
                   m_CPClearReg.Hex = tmp.Hex;
                   UpdateInterrupts();
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
                   if (fifo.CPReadWriteDistance == 0)
                   {
                     GPFifo::ResetGatherPipe();
                     Fifo::ResetVideoBuffer();
                   }
                   else
                   {
                     Fifo::ResetVideoBuffer();
                   }
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
  {
    if (SConfig::GetInstance().bCPUThread && !Fifo::UseDeterministicGPUThread())
    {
      // In multibuffer mode is not allowed write in the same FIFO attached to the GPU.
      // Fix Pokemon XD in DC mode.
      if ((ProcessorInterface::Fifo_CPUEnd == fifo.CPEnd) &&
          (ProcessorInterface::Fifo_CPUBase == fifo.CPBase) && fifo.CPReadWriteDistance > 0)
      {
        Fifo::SyncGPU(Fifo::SyncGPUReason::Other);
      }
    }
    return;
  }

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

// TODO: Unify this with Fifo.cpp
static constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;

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
  UpdateInterrupts();
  if (!CanRun())
    return;
  do
  {
    // Work out the copy size. We can copy up until the next interrupt, or breakpoint.
    u32 copy_size =
        std::min(std::min(fifo.CPReadWriteDistance, fifo.CPEnd - fifo.CPReadPointer), FIFO_SIZE);
    if (fifo.CPReadWriteDistance < fifo.CPHiWatermark)
      copy_size = std::min(copy_size, fifo.CPHiWatermark - fifo.CPReadWriteDistance);
    if (fifo.CPReadWriteDistance > fifo.CPLoWatermark)
      copy_size = std::min(copy_size, fifo.CPReadWriteDistance - fifo.CPLoWatermark);
    if (fifo.bFF_BPEnable && fifo.CPReadPointer < fifo.CPBreakpoint)
      copy_size = std::min(copy_size, fifo.CPBreakpoint - fifo.CPReadPointer);
    if (fifo.CPReadPointer == fifo.CPEnd)
      copy_size = GATHER_PIPE_SIZE;

    // Ensure copy_size is aligned to 32 bytes. It should be...
    copy_size = (copy_size + 31u) & ~31u;
    Fifo::ReadDataFromFifo(fifo.CPReadPointer, copy_size);

    // libogc says "Due to the mechanics of flushing the write-gather pipe, the FIFO memory area
    // should be at least 32 bytes larger than the maximum expected amount of data stored". Hence
    // why we do this check after the read. Also see GPFifo.cpp.
    if (fifo.CPReadPointer == fifo.CPEnd)
      fifo.CPReadPointer = fifo.CPBase;
    else
      fifo.CPReadPointer += copy_size;

    fifo.CPReadWriteDistance -= copy_size;

    UpdateInterrupts();
  } while (CanRun());

  Fifo::WakeGpu();
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
               fifo.bFF_GPReadEnable ? "true" : "false", fifo.bFF_BPEnable ? "true" : "false",
               fifo.bFF_BPInt ? "true" : "false", fifo.bFF_Breakpoint ? "true" : "false",
               fifo.bFF_GPLinkEnable ? "true" : "false", fifo.bFF_HiWatermarkInt ? "true" : "false",
               fifo.bFF_LoWatermarkInt ? "true" : "false");
  }
}

}  // end of namespace CommandProcessor
