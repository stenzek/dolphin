// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"

class PointerWrap;
namespace MMIO
{
class Mapping;
}

namespace CommandProcessor
{
struct SCPFifoStruct
{
  // fifo registers
  volatile u32 CPBase;
  volatile u32 CPEnd;
  u32 CPHiWatermark;
  u32 CPLoWatermark;
  volatile u32 CPReadWriteDistance;
  volatile u32 CPWritePointer;
  volatile u32 CPReadPointer;
  volatile u32 CPBreakpoint;
  volatile u32 SafeCPReadPointer;

  volatile u32 GPLinkEnable;
  volatile u32 ReadEnable;
  volatile u32 BreakpointEnable;
  volatile u32 BreakpointInterruptEnable;
  volatile u32 UnderflowInterruptEnable;
  volatile u32 OverflowInterruptEnable;

  volatile u32 BreakpointFlag;
  volatile u32 UnderflowFlag;
  volatile u32 OverflowFlag;

  void DoState(PointerWrap& p);
};

// This one is shared between gfx thread and emulator thread.
// It is only used by the Fifo and by the CommandProcessor.
extern SCPFifoStruct fifo;

// internal hardware addresses
enum
{
  STATUS_REGISTER = 0x00,
  CTRL_REGISTER = 0x02,
  CLEAR_REGISTER = 0x04,
  PERF_SELECT = 0x06,
  FIFO_TOKEN_REGISTER = 0x0E,
  FIFO_BOUNDING_BOX_LEFT = 0x10,
  FIFO_BOUNDING_BOX_RIGHT = 0x12,
  FIFO_BOUNDING_BOX_TOP = 0x14,
  FIFO_BOUNDING_BOX_BOTTOM = 0x16,
  FIFO_BASE_LO = 0x20,
  FIFO_BASE_HI = 0x22,
  FIFO_END_LO = 0x24,
  FIFO_END_HI = 0x26,
  FIFO_HI_WATERMARK_LO = 0x28,
  FIFO_HI_WATERMARK_HI = 0x2a,
  FIFO_LO_WATERMARK_LO = 0x2c,
  FIFO_LO_WATERMARK_HI = 0x2e,
  FIFO_RW_DISTANCE_LO = 0x30,
  FIFO_RW_DISTANCE_HI = 0x32,
  FIFO_WRITE_POINTER_LO = 0x34,
  FIFO_WRITE_POINTER_HI = 0x36,
  FIFO_READ_POINTER_LO = 0x38,
  FIFO_READ_POINTER_HI = 0x3A,
  FIFO_BP_LO = 0x3C,
  FIFO_BP_HI = 0x3E,
  XF_RASBUSY_L = 0x40,
  XF_RASBUSY_H = 0x42,
  XF_CLKS_L = 0x44,
  XF_CLKS_H = 0x46,
  XF_WAIT_IN_L = 0x48,
  XF_WAIT_IN_H = 0x4a,
  XF_WAIT_OUT_L = 0x4c,
  XF_WAIT_OUT_H = 0x4e,
  VCACHE_METRIC_CHECK_L = 0x50,
  VCACHE_METRIC_CHECK_H = 0x52,
  VCACHE_METRIC_MISS_L = 0x54,
  VCACHE_METRIC_MISS_H = 0x56,
  VCACHE_METRIC_STALL_L = 0x58,
  VCACHE_METRIC_STALL_H = 0x5A,
  CLKS_PER_VTX_IN_L = 0x60,
  CLKS_PER_VTX_IN_H = 0x62,
  CLKS_PER_VTX_OUT = 0x64,
};

enum
{
  GATHER_PIPE_SIZE = 32,
  INT_CAUSE_CP = 0x800
};

// Fifo Status Register
union UCPStatusReg
{
  struct
  {
    u16 Overflow : 1;
    u16 Underflow : 1;
    u16 ReadIdle : 1;
    u16 CommandIdle : 1;
    u16 Breakpoint : 1;
    u16 : 11;
  };
  u16 Hex;
  UCPStatusReg() { Hex = 0; }
  UCPStatusReg(u16 _hex) { Hex = _hex; }
};

// Fifo Control Register
union UCPCtrlReg
{
  struct
  {
    u16 ReadEnable : 1;
    u16 BPEnable : 1;
    u16 OverflowIntEnable : 1;
    u16 UnderflowIntEnable : 1;
    u16 GPLinkEnable : 1;
    u16 BPInt : 1;
    u16 : 10;
  };
  u16 Hex;
  UCPCtrlReg() { Hex = 0; }
  UCPCtrlReg(u16 _hex) { Hex = _hex; }
};

// Fifo Clear Register
union UCPClearReg
{
  struct
  {
    u16 ClearFifoOverflow : 1;
    u16 ClearFifoUnderflow : 1;
    u16 ClearMetrices : 1;
    u16 : 13;
  };
  u16 Hex;
  UCPClearReg() { Hex = 0; }
  UCPClearReg(u16 _hex) { Hex = _hex; }
};

// Init
void Init();
void DoState(PointerWrap& p);

void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

// Checking whether we can run, due to breakpoints and R/W distance.
bool AtBreakpoint();
bool CanReadFromFifo();

void SetCPStatusFromGPU();
void SetCPStatusFromCPU();
void GatherPipeBursted();
void UpdateInterrupts(u64 userdata);
void UpdateInterruptsFromVideoBackend(u64 userdata);

bool IsInterruptWaiting();

void SetCpClearRegister();
void SetCpControlRegister();
void SetCpStatusRegister();

void HandleUnknownOpcode(u8 cmd_byte, void* buffer, bool preprocess);

}  // namespace CommandProcessor
