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
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"

#define CP_DEBUG_LOG(...) WARN_LOG(VIDEO, __VA_ARGS__)

#ifndef CP_DEBUG_LOG
#define CP_DEBUG_LOG(...)                                                                          \
  do                                                                                               \
  {                                                                                                \
  } while (0)
#endif

namespace CommandProcessor
{
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

// Updating registers with FIFO state.
static void OnClearRegisterSet();

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

bool AtBreakpoint()
{
  return m_CPCtrlReg.BPEnable && (fifo.CPReadPointer == fifo.CPBreakpoint);
}

bool CanReadFromFifo()
{
  return m_CPCtrlReg.GPReadEnable && fifo.CPReadWriteDistance >= GATHER_PIPE_SIZE &&
         !AtBreakpoint();
}

void Init()
{
  m_CPStatusReg.Hex = 0;
  m_CPCtrlReg.Hex = 0;
  m_CPClearReg.Hex = 0;

  m_bboxleft = 0;
  m_bboxtop = 0;
  m_bboxright = 640;
  m_bboxbottom = 480;

  m_tokenReg = 0;

  memset(&fifo, 0, sizeof(fifo));

  s_interrupt_set.Clear();
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // notes:
  // rwdistance - high -> low
  // bp - low -> high

  constexpr u16 WMASK_NONE = 0x0000;
  constexpr u16 WMASK_ALL = 0xffff;
  constexpr u16 WMASK_LO_ALIGN_32BIT = 0xffe0;
  const u16 WMASK_HI_RESTRICT = SConfig::GetInstance().bWii ? 0x1fff : 0x03ff;

  struct
  {
    u32 addr;
    u16* ptr;
    bool readonly;
    bool sync;
    // FIFO mmio regs in the range [cc000020-cc00003e] have certain bits that always read as 0
    // For _LO registers in this range, only bits 0xffe0 can be set
    // For _HI registers in this range, only bits 0x03ff can be set on GCN and 0x1fff on Wii
    u16 wmask;
  } directly_mapped_vars[] = {
      {FIFO_TOKEN_REGISTER, &m_tokenReg, false, false, WMASK_ALL},

      // Bounding box registers are read only.
      {FIFO_BOUNDING_BOX_LEFT, &m_bboxleft, true, false, WMASK_NONE},
      {FIFO_BOUNDING_BOX_RIGHT, &m_bboxright, true, false, WMASK_NONE},
      {FIFO_BOUNDING_BOX_TOP, &m_bboxtop, true, false, WMASK_NONE},
      {FIFO_BOUNDING_BOX_BOTTOM, &m_bboxbottom, true, false, WMASK_NONE},
      {FIFO_BASE_LO, MMIO::Utils::LowPart(&fifo.CPBase), false, true, WMASK_LO_ALIGN_32BIT},
      {FIFO_BASE_HI, MMIO::Utils::HighPart(&fifo.CPBase), false, true, WMASK_HI_RESTRICT},
      {FIFO_END_LO, MMIO::Utils::LowPart(&fifo.CPEnd), false, true, WMASK_LO_ALIGN_32BIT},
      {FIFO_END_HI, MMIO::Utils::HighPart(&fifo.CPEnd), false, true, WMASK_HI_RESTRICT},
      {FIFO_HI_WATERMARK_LO, MMIO::Utils::LowPart(&fifo.CPHiWatermark), false, true,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_HI_WATERMARK_HI, MMIO::Utils::HighPart(&fifo.CPHiWatermark), false, true,
       WMASK_HI_RESTRICT},
      {FIFO_LO_WATERMARK_LO, MMIO::Utils::LowPart(&fifo.CPLoWatermark), false, true,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_LO_WATERMARK_HI, MMIO::Utils::HighPart(&fifo.CPLoWatermark), false, true,
       WMASK_HI_RESTRICT},
      // FIFO_RW_DISTANCE has some complex read code different for
      // single/dual core.
      {FIFO_WRITE_POINTER_LO, MMIO::Utils::LowPart(&fifo.CPWritePointer), false, true,
       WMASK_LO_ALIGN_32BIT},
      {FIFO_WRITE_POINTER_HI, MMIO::Utils::HighPart(&fifo.CPWritePointer), false, true,
       WMASK_HI_RESTRICT},
      // FIFO_READ_POINTER has different code for single/dual core.
  };

  for (auto& mapped_var : directly_mapped_vars)
  {
    u16* const ptr = mapped_var.ptr;
    u32 const wmask = mapped_var.wmask;
    mmio->Register(base | mapped_var.addr,
                   mapped_var.sync ? MMIO::ComplexRead<u16>([ptr](u32) {
                     Fifo::SyncGPUForRegisterAccess(false);
                     return *ptr;
                   }) :
                                     MMIO::DirectRead<u16>(ptr),
                   mapped_var.readonly ?
                       MMIO::InvalidWrite<u16>() :
                       (mapped_var.sync ?
                            MMIO::ComplexWrite<u16>([ptr, wmask](u32, u16 val) {
                              Fifo::SyncGPUForRegisterAccess(true);
                              *ptr = val & wmask;
                            }) :
                            MMIO::DirectWrite<u16>(mapped_var.ptr, mapped_var.wmask)));
  }

  mmio->Register(base | FIFO_BP_LO, MMIO::ComplexRead<u16>([](u32) {
                   Fifo::SyncGPUForRegisterAccess(false);
                   return ReadLow(fifo.CPBreakpoint);
                 }),
                 MMIO::ComplexWrite<u16>([WMASK_LO_ALIGN_32BIT](u32, u16 val) {
                   Fifo::SyncGPUForRegisterAccess(true);
                   WriteLow(fifo.CPBreakpoint, val & WMASK_LO_ALIGN_32BIT);
                 }));
  mmio->Register(base | FIFO_BP_HI, MMIO::ComplexRead<u16>([](u32) {
                   Fifo::SyncGPUForRegisterAccess(false);
                   return ReadHigh(fifo.CPBreakpoint);
                 }),
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

  mmio->Register(
      base | STATUS_REGISTER, MMIO::ComplexRead<u16>([](u32) {
        Fifo::SyncGPUForRegisterAccess(false);
        CP_DEBUG_LOG(
            "Read from STATUS_REGISTER: iBP %s | ReadIdle %s | CmdIdle %s | OvF %s | UndF %s",
            m_CPStatusReg.Breakpoint ? "ON" : "OFF", m_CPStatusReg.ReadIdle ? "ON" : "OFF",
            m_CPStatusReg.CommandIdle ? "ON" : "OFF",
            m_CPStatusReg.OverflowHiWatermark ? "ON" : "OFF",
            m_CPStatusReg.UnderflowLoWatermark ? "ON" : "OFF");
        return m_CPStatusReg.Hex;
      }),
      MMIO::InvalidWrite<u16>());

  mmio->Register(
      base | CTRL_REGISTER, MMIO::DirectRead<u16>(&m_CPCtrlReg.Hex),
      MMIO::ComplexWrite<u16>([](u32, u16 val) {
        Fifo::SyncGPUForRegisterAccess(true);
        UCPCtrlReg tmp(val);
        m_CPCtrlReg.Hex = tmp.Hex;

        CP_DEBUG_LOG("CONTROL_REGISTER WRITE GPREAD %s | BP %s | Int %s | OvF %s | UndF "
                     "%s | LINK %s",
                     m_CPCtrlReg.GPReadEnable ? "ON" : "OFF", m_CPCtrlReg.BPEnable ? "ON" : "OFF",
                     m_CPCtrlReg.BPInt ? "ON" : "OFF", m_CPCtrlReg.OverflowIntEnable ? "ON" : "OFF",
                     m_CPCtrlReg.UnderflowIntEnable ? "ON" : "OFF",
                     m_CPCtrlReg.GPLinkEnable ? "ON" : "OFF");

        UpdateStatusFlags();
        Fifo::UpdateGPUSuspendState();
      }));

  mmio->Register(base | CLEAR_REGISTER, MMIO::DirectRead<u16>(&m_CPClearReg.Hex),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Fifo::SyncGPUForRegisterAccess(true);
                   UCPClearReg tmp(val);
                   m_CPClearReg.Hex = tmp.Hex;
                   OnClearRegisterSet();
                 }));

  mmio->Register(base | PERF_SELECT, MMIO::InvalidRead<u16>(), MMIO::Nop<u16>());

  mmio->Register(base | FIFO_RW_DISTANCE_LO, MMIO::ComplexRead<u16>([](u32) {
                   return ReadLow(fifo.CPReadWriteDistance);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Fifo::SyncGPUForRegisterAccess(true);
                   WriteLow(fifo.CPReadWriteDistance, val & 0xFFE0);
                   CP_DEBUG_LOG("Write RW Distance LOW %u", fifo.CPReadWriteDistance);

                   // TODO: Is this correct? libogc would suggest this happens by writing to WPAR.
                   if (fifo.CPReadWriteDistance == 0)
                     GPFifo::ResetGatherPipe();

                   Fifo::UpdateGPUSuspendState();
                   UpdateOverflowUnderflowFlags();
                   UpdateStatusFlags();
                 }));
  mmio->Register(base | FIFO_RW_DISTANCE_HI, MMIO::ComplexRead<u16>([](u32) {
                   Fifo::SyncGPUForRegisterAccess(false);
                   return ReadHigh(fifo.CPReadWriteDistance);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   WriteHigh(fifo.CPReadWriteDistance, val);
                   CP_DEBUG_LOG("Write RW Distance HI %u", fifo.CPReadWriteDistance);
                 }));
  mmio->Register(base | FIFO_READ_POINTER_LO, MMIO::ComplexRead<u16>([](u32) {
                   Fifo::SyncGPUForRegisterAccess(false);
                   return ReadLow(fifo.CPReadPointer);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Fifo::SyncGPUForRegisterAccess(true);
                   WriteLow(fifo.CPReadPointer, val & 0xFFE0);
                   CP_DEBUG_LOG("Write Read Pointer LO %08X", fifo.CPReadPointer);
                 }));
  mmio->Register(base | FIFO_READ_POINTER_HI, MMIO::ComplexRead<u16>([](u32) {
                   Fifo::SyncGPUForRegisterAccess(false);
                   return ReadHigh(fifo.CPReadPointer);
                 }),
                 MMIO::ComplexWrite<u16>([](u32, u16 val) {
                   Fifo::SyncGPUForRegisterAccess(true);
                   WriteHigh(fifo.CPReadPointer, val);
                   CP_DEBUG_LOG("Write Read Pointer HI %08X", fifo.CPReadPointer);
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

  CP_DEBUG_LOG("GPBurst: RP=%08X, WP=%08X, RWD=%u, VBS=%u", fifo.CPReadPointer, fifo.CPWritePointer,
               fifo.CPReadWriteDistance, Fifo::video_buffer_size.load());

  if (m_CPCtrlReg.GPLinkEnable)
  {
    ProcessorInterface::Fifo_CPUWritePointer = fifo.CPWritePointer;
    ProcessorInterface::Fifo_CPUBase = fifo.CPBase;
    ProcessorInterface::Fifo_CPUEnd = fifo.CPEnd;
  }

  // If this write will exceed the high watermark, run the GPU before incrementing the distance.
  // This way, the interrupt only fires when there is a true overflow, and not just because the
  // last GPU sync was a while ago.
  if (m_CPCtrlReg.OverflowIntEnable && !m_CPStatusReg.OverflowHiWatermark &&
      fifo.CPReadWriteDistance >= fifo.CPHiWatermark)
  {
    Fifo::SyncGPUForRegisterAccess(true);
    UpdateOverflowUnderflowFlags();
    UpdateStatusFlags();
    UpdateInterrupts();
  }

  // The FIFO can be overflowed by the gatherpipe, if a large number of bytes is written in a
  // single JIT block. In this case, "borrow" some cycles to execute the GPU now instead of later,
  // reducing the amount of data in the FIFO. In dual core, we take this even further, kicking the
  // GPU as soon as there is half a kilobyte of commands. of commands, before kicking the GPU.
  // Kicking on every GP burst is too slow, and waiting too long introduces latency when we
  // eventually do need to synchronize with the GPU thread.
  if (SConfig::GetInstance().bCPUThread)
  {
    if (fifo.CPReadWriteDistance >= Fifo::FIFO_EXECUTE_THRESHOLD_SIZE)
      Fifo::RunGpu(true);
  }
  else
  {
    if (fifo.CPReadWriteDistance >= fifo.CPEnd - fifo.CPBase)
      Fifo::RunGpu(true);
  }

  // Even if we don't have sufficient work now, make sure the GPU is awake (syncing enabled).
  Fifo::UpdateGPUSuspendState();

  if (fifo.CPReadWriteDistance >= (fifo.CPEnd - fifo.CPBase) && !CanReadFromFifo())
    PanicAlert("FIFO is completely stuffed");

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

void UpdateBreakpointFlag()
{
  // breakpoint
  if (AtBreakpoint())
  {
    if (!m_CPStatusReg.Breakpoint)
    {
      CP_DEBUG_LOG("Hit breakpoint at %i", fifo.CPReadPointer);
      m_CPStatusReg.Breakpoint = true;
    }
  }
  else
  {
    if (m_CPStatusReg.Breakpoint)
    {
      CP_DEBUG_LOG("Cleared breakpoint at %i", fifo.CPReadPointer);
      m_CPStatusReg.Breakpoint = false;
    }
  }
}

void UpdateOverflowUnderflowFlags()
{
  // overflow & underflow check
  if (fifo.CPReadWriteDistance >= fifo.CPHiWatermark && m_CPCtrlReg.OverflowIntEnable)
  {
    if (!m_CPStatusReg.OverflowHiWatermark)
    {
      CP_DEBUG_LOG("Set overflow at %08X %u", fifo.CPReadPointer, fifo.CPReadWriteDistance);
      m_CPStatusReg.OverflowHiWatermark = true;
    }
  }

  if (fifo.CPReadWriteDistance <= fifo.CPLoWatermark && m_CPCtrlReg.UnderflowIntEnable)
  {
    if (!m_CPStatusReg.UnderflowLoWatermark)
    {
      CP_DEBUG_LOG("Set underflow at %08X %u", fifo.CPReadPointer, fifo.CPReadWriteDistance);
      m_CPStatusReg.UnderflowLoWatermark = true;
    }
  }
}

void UpdateStatusFlags()
{
  // idle flags
  m_CPStatusReg.ReadIdle = !CanReadFromFifo();
  m_CPStatusReg.CommandIdle = !CanReadFromFifo();
}

void UpdateInterrupts()
{
  const bool has_interrupt = (m_CPCtrlReg.BPInt & m_CPStatusReg.Breakpoint) |
                             (m_CPCtrlReg.OverflowIntEnable & m_CPStatusReg.OverflowHiWatermark) |
                             (m_CPCtrlReg.UnderflowIntEnable & m_CPStatusReg.UnderflowLoWatermark);

  if (has_interrupt)
  {
    if (s_interrupt_set.TestAndSet())
    {
      CP_DEBUG_LOG("Interrupt set");
      ProcessorInterface::SetInterrupt(INT_CAUSE_CP, true);
      CoreTiming::ForceExceptionCheck(0);
    }
  }
  else
  {
    if (s_interrupt_set.TestAndClear())
    {
      CP_DEBUG_LOG("Interrupt cleared");
      ProcessorInterface::SetInterrupt(INT_CAUSE_CP, false);
    }
  }
}

u32 CopyToVideoBuffer(u32 maximum_copy_size)
{
  u32 bytes_copied = 0;

  // we can't go past the GPU thread..
  maximum_copy_size = std::min(maximum_copy_size, Fifo::FIFO_SIZE - Fifo::video_buffer_size.load());
  if (!CanReadFromFifo() || maximum_copy_size == 0)
    return 0;

  do
  {
    // Work out the copy size. We can copy up until the next breakpoint, or the FIFO wraps around.
    // This should always be 32-byte aligned.

    // libogc says "Due to the mechanics of flushing the write-gather pipe, the FIFO memory area
    // should be at least 32 bytes larger than the maximum expected amount of data stored". Hence
    // why we do this check after the read. Also see GPFifo.cpp.
    u32 copy_size =
        std::min(maximum_copy_size, std::min(fifo.CPReadWriteDistance,
                                             fifo.CPEnd - fifo.CPReadPointer + GATHER_PIPE_SIZE));
    if (m_CPCtrlReg.BPEnable && fifo.CPReadPointer < fifo.CPBreakpoint)
      copy_size = std::min(copy_size, fifo.CPBreakpoint - fifo.CPReadPointer);

    ASSERT(fifo.CPReadWriteDistance >= copy_size);
    ASSERT_MSG(VIDEO, (copy_size % GATHER_PIPE_SIZE) == 0,
               "read size is not aligned to gather pipe size");

    // Handle video buffer wrapping around.
    const u8* video_buffer_end_ptr = Fifo::video_buffer + Fifo::FIFO_SIZE;
    if ((Fifo::video_buffer_write_ptr + copy_size) > video_buffer_end_ptr)
    {
      std::lock_guard<std::mutex> guard(Fifo::video_buffer_mutex);

      const u32 remaining_data =
          static_cast<u32>(Fifo::video_buffer_write_ptr - Fifo::video_buffer_read_ptr);
      if (remaining_data > 0)
      {
        // move anything left over to the start
        std::memmove(Fifo::video_buffer, Fifo::video_buffer_read_ptr, remaining_data);
      }
      Fifo::video_buffer_read_ptr = Fifo::video_buffer;
      Fifo::video_buffer_write_ptr = Fifo::video_buffer + remaining_data;
    }
    if (Fifo::video_buffer_size.load() == 0)
    {
      Fifo::video_buffer_read_ptr = Fifo::video_buffer;
      Fifo::video_buffer_write_ptr = Fifo::video_buffer;
    }

    // Copy new video instructions to s_video_buffer for future use in rendering the new picture
    Memory::CopyFromEmu(Fifo::video_buffer_write_ptr, fifo.CPReadPointer, copy_size);
#if 1
    for (u32 i = 0; i < copy_size; i += 32)
    {
      u8* vb = Fifo::video_buffer_write_ptr + i;
      CP_DEBUG_LOG(
          "FIFO: %08X [%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
          "%02X "
          "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
          fifo.CPReadPointer + i, vb[0], vb[1], vb[2], vb[3], vb[4], vb[5], vb[6], vb[7], vb[8],
          vb[9], vb[10], vb[11], vb[12], vb[13], vb[14], vb[15], vb[15], vb[16], vb[17], vb[18],
          vb[19], vb[20], vb[21], vb[22], vb[23], vb[24], vb[25], vb[26], vb[27], vb[28], vb[29],
          vb[30], vb[31]);
    }
#endif
    Fifo::video_buffer_write_ptr += copy_size;
    Fifo::video_buffer_size.fetch_add(copy_size);

    // Update FIFO read pointer. The actual end of the buffer is 32 bytes beyond the end pointer.
    fifo.CPReadPointer += copy_size;
    if (fifo.CPReadPointer == (fifo.CPEnd + GATHER_PIPE_SIZE))
      fifo.CPReadPointer = fifo.CPBase;

    fifo.CPReadWriteDistance -= copy_size;
    maximum_copy_size -= copy_size;
    bytes_copied += copy_size;
  } while (CanReadFromFifo() && maximum_copy_size > 0);

  UpdateBreakpointFlag();
  UpdateOverflowUnderflowFlags();
  UpdateInterrupts();
  UpdateStatusFlags();

  CP_DEBUG_LOG("CopyToVideoBuffer -> %u bytes, new RWD: %u", bytes_copied,
               fifo.CPReadWriteDistance);
  return bytes_copied;
}

u32 GetCPBaseRegister()
{
  return fifo.CPBase;
}

u32 GetCPEndRegister()
{
  return fifo.CPEnd;
}

void OnClearRegisterSet()
{
  if (m_CPClearReg.ClearFifoOverflow != 0)
  {
    CP_DEBUG_LOG("CLEARED overflow interrupt at %08X %u", fifo.CPReadPointer,
                 fifo.CPReadWriteDistance);
    m_CPStatusReg.OverflowHiWatermark = false;
  }
  if (m_CPClearReg.ClearFifoUnderflow != 0)
  {
    CP_DEBUG_LOG("CLEARED underflow interrupt at %08X %u", fifo.CPReadPointer,
                 fifo.CPReadWriteDistance);
    m_CPStatusReg.UnderflowLoWatermark = false;
  }

  UpdateInterrupts();
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
               "GPReadEnable: %s\n"
               "GPLinkEnable: %s\n"
               "BPEnable: %s\n"
               "BPInt: %s\n"
               "OverflowIntEnable: %s\n"
               "UnderflowIntEnable: %s\n"
               "Breakpoint: %s\n"
               "OverflowHiWatermark: %s\n"
               "UnderflowLoWatermark: %s\n",
               cmd_byte, fifo.CPBase, fifo.CPEnd, fifo.CPHiWatermark, fifo.CPLoWatermark,
               fifo.CPReadWriteDistance, fifo.CPWritePointer, fifo.CPReadPointer, fifo.CPBreakpoint,
               m_CPCtrlReg.GPReadEnable ? "true" : "false",
               m_CPCtrlReg.GPLinkEnable ? "true" : "false", m_CPCtrlReg.BPEnable ? "true" : "false",
               m_CPCtrlReg.BPInt ? "true" : "false",
               m_CPCtrlReg.OverflowIntEnable ? "true" : "false",
               m_CPCtrlReg.UnderflowIntEnable ? "true" : "false",
               m_CPStatusReg.Breakpoint ? "true" : "false",
               m_CPStatusReg.OverflowHiWatermark ? "true" : "false",
               m_CPStatusReg.UnderflowLoWatermark ? "true" : "false");
  }
}

u32 GetCPReadWriteDistance()
{
  return fifo.CPReadWriteDistance;
}

}  // end of namespace CommandProcessor
