// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// This is currently only used by the DX backend, but it may make sense to
// use it in the GL backend or a future DX10 backend too.

#pragma once

#include "Common/CommonTypes.h"

class IndexGenerator
{
public:
  // Init
  static void Init();
  static void Start(u16* start_ptr, u16* end_ptr);

  static void AddIndices(int primitive, u32 numVertices);

  static void AddExternalIndices(const u16* indices, u32 num_indices, u32 num_vertices);

  // returns numprimitives
  static u32 GetNumVerts() { return base_index; }
  static u32 GetIndexLen() { return static_cast<u32>(m_current_pointer - m_start_pointer); }
  static u32 GetFreeBytes()
  {
    return static_cast<u32>(reinterpret_cast<u8*>(m_end_pointer) -
                            reinterpret_cast<u8*>(m_current_pointer));
  }
  static u32 GetRemainingIndices();

private:
  // Triangles
  template <bool pr>
  static u16* AddList(u16* Iptr, u32 numVerts, u32 index);
  template <bool pr>
  static u16* AddStrip(u16* Iptr, u32 numVerts, u32 index);
  template <bool pr>
  static u16* AddFan(u16* Iptr, u32 numVerts, u32 index);
  template <bool pr>
  static u16* AddQuads(u16* Iptr, u32 numVerts, u32 index);
  template <bool pr>
  static u16* AddQuads_nonstandard(u16* Iptr, u32 numVerts, u32 index);

  // Lines
  static u16* AddLineList(u16* Iptr, u32 numVerts, u32 index);
  static u16* AddLineStrip(u16* Iptr, u32 numVerts, u32 index);

  // Points
  static u16* AddPoints(u16* Iptr, u32 numVerts, u32 index);

  template <bool pr>
  static u16* WriteTriangle(u16* Iptr, u32 index1, u32 index2, u32 index3);

  static u16* m_current_pointer;
  static u16* m_start_pointer;
  static u16* m_end_pointer;
  static u32 base_index;
};
