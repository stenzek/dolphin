// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// DL facts:
//  Ikaruga uses (nearly) NO display lists!
//  Zelda WW uses TONS of display lists
//  Zelda TP uses almost 100% display lists except menus (we like this!)
//  Super Mario Galaxy has nearly all geometry and more than half of the state in DLs (great!)

// Note that it IS NOT GENERALLY POSSIBLE to precompile display lists! You can compile them as they
// are
// while interpreting them, and hope that the vertex format doesn't change, though, if you do it
// right
// when they are called. The reason is that the vertex format affects the sizes of the vertices.

#include "VideoCommon/OpcodeDecoding.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/Memmap.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

bool g_bRecordFifoData = false;

namespace OpcodeDecoder
{
static bool s_bFifoErrorSeen = false;

static u32 InterpretDisplayList(u32 address, u32 size)
{
  u8* startAddress = Memory::GetPointer(address);
  u32 cycles = 0;

  // Avoid the crash if Memory::GetPointer failed ..
  if (startAddress != nullptr)
  {
    // temporarily swap dl and non-dl (small "hack" for the stats)
    Statistics::SwapDL();

    Run(DataReader(startAddress, startAddress + size), &cycles, true);
    INCSTAT(stats.thisFrame.numDListsCalled);

    // un-swap
    Statistics::SwapDL();
  }

  return cycles;
}

void Init()
{
  s_bFifoErrorSeen = false;
}

u8* Run(DataReader src, u32* cycles, bool in_display_list)
{
  u32 totalCycles = 0;
  u8* opcodeStart;
  while (true)
  {
    opcodeStart = src.GetPointer();

    if (!src.size())
      goto end;

    u8 cmd_byte = src.Read<u8>();
    int refarray;
    switch (cmd_byte)
    {
    case GX_NOP:
      totalCycles += 6;  // Hm, this means that we scan over nop streams pretty slowly...
      break;

    case GX_UNKNOWN_RESET:
      totalCycles += 6;  // Datel software uses this command
      DEBUG_LOG(VIDEO, "GX Reset?: %08x", cmd_byte);
      break;

    case GX_LOAD_CP_REG:
    {
      if (src.size() < 1 + 4)
        goto end;
      totalCycles += 12;
      u8 sub_cmd = src.Read<u8>();
      u32 value = src.Read<u32>();
      LoadCPReg(sub_cmd, value);
      INCSTAT(stats.thisFrame.numCPLoads);
    }
    break;

    case GX_LOAD_XF_REG:
    {
      if (src.size() < 4)
        goto end;
      u32 Cmd2 = src.Read<u32>();
      int transfer_size = ((Cmd2 >> 16) & 15) + 1;
      if (src.size() < transfer_size * sizeof(u32))
        goto end;
      totalCycles += 18 + 6 * transfer_size;

      u32 xf_address = Cmd2 & 0xFFFF;
      LoadXFReg(transfer_size, xf_address, src);
      INCSTAT(stats.thisFrame.numXFLoads);

      src.Skip<u32>(transfer_size);
    }
    break;

    case GX_LOAD_INDX_A:  // used for position matrices
      refarray = 0xC;
      goto load_indx;
    case GX_LOAD_INDX_B:  // used for normal matrices
      refarray = 0xD;
      goto load_indx;
    case GX_LOAD_INDX_C:  // used for postmatrices
      refarray = 0xE;
      goto load_indx;
    case GX_LOAD_INDX_D:  // used for lights
      refarray = 0xF;
      goto load_indx;
    load_indx:
      if (src.size() < 4)
        goto end;
      totalCycles += 6;
      LoadIndexedXF(src.Read<u32>(), refarray);
      break;

    case GX_CMD_CALL_DL:
    {
      if (src.size() < 8)
        goto end;
      u32 address = src.Read<u32>();
      u32 count = src.Read<u32>();

      if (in_display_list)
      {
        totalCycles += 6;
        INFO_LOG(VIDEO, "recursive display list detected");
      }
      else
      {
        totalCycles += 6 + InterpretDisplayList(address, count);
      }
    }
    break;

    case GX_CMD_UNKNOWN_METRICS:  // zelda 4 swords calls it and checks the metrics registers after
                                  // that
      totalCycles += 6;
      DEBUG_LOG(VIDEO, "GX 0x44: %08x", cmd_byte);
      break;

    case GX_CMD_INVL_VC:  // Invalidate Vertex Cache
      totalCycles += 6;
      DEBUG_LOG(VIDEO, "Invalidate (vertex cache?)");
      break;

    case GX_LOAD_BP_REG:
      // In skipped_frame case: We have to let BP writes through because they set
      // tokens and stuff.  TODO: Call a much simplified LoadBPReg instead.
      {
        if (src.size() < 4)
          goto end;
        totalCycles += 12;
        u32 bp_cmd = src.Read<u32>();
        LoadBPReg(bp_cmd);
        INCSTAT(stats.thisFrame.numBPLoads);
      }
      break;

    // draw primitives
    default:
      if ((cmd_byte & 0xC0) == 0x80)
      {
        // load vertices
        if (src.size() < 2)
          goto end;
        u16 num_vertices = src.Read<u16>();
        int bytes = VertexLoaderManager::RunVertices(
            cmd_byte & GX_VAT_MASK,  // Vertex loader index (0 - 7)
            (cmd_byte & GX_PRIMITIVE_MASK) >> GX_PRIMITIVE_SHIFT, num_vertices, src);

        if (bytes < 0)
          goto end;

        src.Skip(bytes);

        // 4 GPU ticks per vertex, 3 CPU ticks per GPU tick
        totalCycles += num_vertices * 4 * 3 + 6;
      }
      else
      {
        if (!s_bFifoErrorSeen)
          Fifo::HandleUnknownOpcode(cmd_byte, opcodeStart);
        ERROR_LOG(VIDEO, "FIFO: Unknown Opcode(0x%02x @ %p)", cmd_byte, opcodeStart);
        s_bFifoErrorSeen = true;
        totalCycles += 1;
      }
      break;
    }

    // Display lists get added directly into the FIFO stream
    if (g_bRecordFifoData && cmd_byte != GX_CMD_CALL_DL)
    {
      u8* opcodeEnd = src.GetPointer();
      FifoRecorder::GetInstance().WriteGPCommand(opcodeStart, u32(opcodeEnd - opcodeStart));
    }
  }

end:
  if (cycles)
  {
    *cycles = totalCycles;
  }
  return opcodeStart;
}

}  // namespace OpcodeDecoder
