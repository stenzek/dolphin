// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"

// all constant buffer attributes must be 16 bytes aligned, so this are the only allowed components:
typedef float float4[4];
typedef u32 uint4[4];
typedef s32 int4[4];

struct PixelShaderConstants
{
  int4 colors[4];
  int4 kcolors[4];
  int4 alpha;
  float4 texdims[8];
  int4 zbias[2];
  int4 indtexscale[2];
  int4 indtexmtx[6];
  int4 fogcolor;
  int4 fogi;
  float4 fogf[2];
  float4 zslope;
  float4 efbscale;
};

struct VertexShaderConstants
{
  u32 components;           // .x
  u32 xfmem_dualTexInfo;    // .y
  u32 xfmem_numColorChans;  // .z
  u32 pad1;                 // .w

  float4 posnormalmatrix[6];
  float4 projection[4];
  int4 materials[4];
  struct Light
  {
    int4 color;
    float4 cosatt;
    float4 distatt;
    float4 pos;
    float4 dir;
  } lights[8];
  float4 texmatrices[24];
  float4 transformmatrices[64];
  float4 normalmatrices[32];
  float4 posttransformmatrices[64];
  float4 pixelcentercorrection;
  float viewport[2];  // .xy
  float pad2[2];      // .zw

  uint4 xfmem_pack1[8];  // .x - texMtxInfo, .y - postMtxInfo, [0..1].z = color, [0..1].w = alpha
};

struct GeometryShaderConstants
{
  float4 stereoparams;
  float4 lineptparams;
  int4 texoffset;
};

struct UberShaderConstants
{
  u32 genmode;       // .x
  u32 alphaTest;     // .y
  u32 fogParam3;     // .z
  u32 fogRangeBase;  // .w
  u32 dstalpha;      // x
  u32 ztex2;         // y
  u32 zcontrol;      // z
  u32 projection;    // w
  uint4 iref;        // .xyzw
  uint4 pack1[16];   // .xy - combiners, .z - tevind
  uint4 pack2[8];    // .x - tevorder, .y - tevksel
  int4 konst[32];    // .rgba
  u32 rgba6_format;  // .x
  u32 dither;        // .y
  u32 bounding_box;  // .z
  u32 pad;           // .w
};
