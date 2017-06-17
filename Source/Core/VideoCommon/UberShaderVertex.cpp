// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/UberShaderVertex.h"
#include "VideoCommon/UberShaderCommon.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace UberShader
{
VertexShaderUid GetVertexShaderUid()
{
  VertexShaderUid out;
  vertex_ubershader_uid_data* uid_data = out.GetUidData<vertex_ubershader_uid_data>();
  memset(uid_data, 0, sizeof(*uid_data));
  uid_data->num_texgens = xfmem.numTexGen.numTexGens;
  return out;
}

ShaderCode GenVertexShader(APIType ApiType, const vertex_ubershader_uid_data* uid_data)
{
  const bool msaa = g_ActiveConfig.iMultisamples > 1;
  const bool ssaa = g_ActiveConfig.iMultisamples > 1 && g_ActiveConfig.bSSAA;
  const u32 numTexgen = uid_data->num_texgens;
  ShaderCode out;

  out.Write("// Vertex UberShader\n\n");
  WriteUberShaderCommonHeader(out, ApiType);

  out.Write("%s", s_lighting_struct);

	// uniforms
  if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
    out.Write("UBO_BINDING(std140, 2) uniform VSBlock {\n");
	else
		out.Write("cbuffer VSBlock {\n");
	out.Write(s_shader_uniforms);
	out.Write("};\n");

  out.WriteLine("int4 CalculateLighting(uint index, uint attnfunc, uint diffusefunc, float4 pos, float3 _norm0) {");
  out.WriteLine("  float3 ldir, h, cosAttn, distAttn;");
  out.WriteLine("  float dist, dist2, attn;");
  out.WriteLine("");
  out.WriteLine("  switch (attnfunc) {");
  out.WriteLine("  case %uu: // LIGNTATTN_NONE", LIGHTATTN_NONE);
  out.WriteLine("  case %uu: // LIGHTATTN_DIR", LIGHTATTN_DIR);
  out.WriteLine("    ldir = normalize(" I_LIGHTS "[index].pos.xyz - pos.xyz);");
  out.WriteLine("    attn = 1.0;");
  out.WriteLine("    if (length(ldir) == 0.0)");
  out.WriteLine("      ldir = _norm0;");
  out.WriteLine("    break;");
  out.WriteLine("  case %uu: // LIGHTATTN_SPEC", LIGHTATTN_SPEC);
  out.WriteLine("    ldir = normalize(" I_LIGHTS "[index].pos.xyz - pos.xyz);");
  out.WriteLine("    attn = (dot(_norm0, ldir) >= 0.0) ? max(0.0, dot(_norm0, " I_LIGHTS "[index].dir.xyz)) : 0.0;");
  out.WriteLine("    cosAttn = " I_LIGHTS "[index].cosatt.xyz;");
  out.WriteLine("    if (diffusefunc == %uu) // LIGHTDIF_NONE", LIGHTDIF_NONE);
  out.WriteLine("      distAttn = " I_LIGHTS "[index].distatt.xyz;");
  out.WriteLine("    else");
  out.WriteLine("      distAttn = normalize(" I_LIGHTS "[index].distatt.xyz);");
  out.WriteLine("    attn = max(0.0, dot(cosAttn, float3(1.0, attn, attn*attn))) / dot(distAttn, float3(1.0, attn, attn*attn));\n");
  out.WriteLine("    break;");
  out.WriteLine("  case %uu: // LIGHTATTN_SPOT", LIGHTATTN_SPOT);
  out.WriteLine("    ldir = " I_LIGHTS "[index].pos.xyz - pos.xyz;");
  out.WriteLine("    dist2 = dot(ldir, ldir);");
  out.WriteLine("    dist = sqrt(dist2);");
  out.WriteLine("    ldir = ldir / dist;");
  out.WriteLine("    attn = max(0.0, dot(ldir, " I_LIGHTS "[index].dir.xyz));");
  out.WriteLine("    attn = max(0.0, " I_LIGHTS "[index].cosatt.x + " I_LIGHTS "[index].cosatt.y * attn + " I_LIGHTS "[index].cosatt.z * attn * attn) / dot(" I_LIGHTS "[index].distatt.xyz, float3(1.0, dist, dist2));");
  out.WriteLine("    break;");
  out.WriteLine("  default:");
  out.WriteLine("    attn = 1.0;");
  out.WriteLine("    ldir = _norm0;");
  out.WriteLine("    break;");
  out.WriteLine("  }");
  out.WriteLine("");
  out.WriteLine("  switch (diffusefunc) {");
  out.WriteLine("  case %uu: // LIGHTDIF_NONE", LIGHTDIF_NONE);
  out.WriteLine("    return int4(round(attn * " I_LIGHTS "[index].color));");
  out.WriteLine("  case %uu: // LIGHTDIF_SIGN", LIGHTDIF_SIGN);
  out.WriteLine("    return int4(round(attn * dot(ldir, _norm0) * " I_LIGHTS "[index].color));");
  out.WriteLine("  case %uu: // LIGHTDIF_CLAMP", LIGHTDIF_CLAMP);
  out.WriteLine("    return int4(round(attn * max(0.0, dot(ldir, _norm0)) * " I_LIGHTS "[index].color));");
  out.WriteLine("  default:");
  out.WriteLine("    return int4(0, 0, 0, 0);");
  out.WriteLine("  }");
  out.WriteLine("}");  

	out.Write("struct VS_OUTPUT {\n");
	GenerateVSOutputMembers(out, ApiType, numTexgen, false, "");
	out.Write("};\n");

	if (ApiType == APIType::OpenGL)
	{
		out.Write("in float4 rawpos; // ATTR%d,\n", SHADER_POSITION_ATTRIB);
		out.Write("in int posmtx; // ATTR%d,\n", SHADER_POSMTX_ATTRIB);
		out.Write("in float3 rawnorm0; // ATTR%d,\n", SHADER_NORM0_ATTRIB);
		out.Write("in float3 rawnorm1; // ATTR%d,\n", SHADER_NORM1_ATTRIB);
		out.Write("in float3 rawnorm2; // ATTR%d,\n", SHADER_NORM2_ATTRIB);

		out.Write("in float4 color0; // ATTR%d,\n", SHADER_COLOR0_ATTRIB);
		out.Write("in float4 color1; // ATTR%d,\n", SHADER_COLOR1_ATTRIB);

		for (int i = 0; i < 8; ++i)
		{
			out.Write("in float3 tex%d; // ATTR%d,\n", i, SHADER_TEXTURE0_ATTRIB + i);
		}

		// TODO: No Geometery shader fallback.
		out.Write("out VertexData {\n");
		GenerateVSOutputMembers(out, ApiType, xfmem.numTexGen.numTexGens, false,
                            GetInterpolationQualifier(msaa, ssaa, true));
		out.Write("} vs;\n");

		out.Write("void main()\n{\n");
	}
	else // D3D
	{
		out.Write("VS_OUTPUT main(\n");

		// inputs
		out.Write("  float3 rawnorm0 : NORMAL0,\n");
		out.Write("  float3 rawnorm1 : NORMAL1,\n");
		out.Write("  float3 rawnorm2 : NORMAL2,\n");
		out.Write("  float4 color0 : COLOR0,\n");
		out.Write("  float4 color1 : COLOR1,\n");
		for (int i = 0; i < 8; ++i)
		{
			out.Write("  float3 tex%d : TEXCOORD%d,\n", i, i);
		}
		out.Write("  int posmtx : BLENDINDICES,\n");
		out.Write("  float4 rawpos : POSITION) {\n");
	}

	out.Write(
		"	VS_OUTPUT o;\n"
		"\n");

	// Transforms
	out.Write(
		"	// Position matrix\n"
		"	float4 P0;\n"
		"	float4 P1;\n"
		"	float4 P2;\n"
		"\n"
		"	// Normal matrix\n"
		"	float3 N0;\n"
		"	float3 N1;\n"
		"	float3 N2;\n"
		"\n"
		"	if ((components & %uu) != 0u) {// VB_HAS_POSMTXIDX\n", VB_HAS_POSMTXIDX);
	out.Write(
		"		// Vertex format has a per-vertex matrix\n"
		"		P0 = " I_TRANSFORMMATRICES"[posmtx];\n"
		"		P1 = " I_TRANSFORMMATRICES"[posmtx+1];\n"
		"		P2 = " I_TRANSFORMMATRICES"[posmtx+2];\n"
		"\n"
		"		int normidx = posmtx >= 32 ? (posmtx - 32) : posmtx;\n"
		"		N0 = " I_NORMALMATRICES"[normidx].xyz;\n"
		"		N1 = " I_NORMALMATRICES"[normidx+1].xyz;\n"
		"		N2 = " I_NORMALMATRICES"[normidx+2].xyz;\n"
		"	} else {\n"
		"		// One shared matrix\n"
		"		P0 = " I_POSNORMALMATRIX"[0];\n"
		"		P1 = " I_POSNORMALMATRIX"[1];\n"
		"		P2 = " I_POSNORMALMATRIX"[2];\n"
		"		N0 = " I_POSNORMALMATRIX"[3].xyz;\n"
		"		N1 = " I_POSNORMALMATRIX"[4].xyz;\n"
		"		N2 = " I_POSNORMALMATRIX"[5].xyz;\n"
		"	}\n"
		"\n"
		"	float4 pos = float4(dot(P0, rawpos), dot(P1, rawpos), dot(P2, rawpos), 1.0);\n"
		"	o.pos = float4(dot(" I_PROJECTION"[0], pos), dot(" I_PROJECTION"[1], pos), dot(" I_PROJECTION"[2], pos), dot(" I_PROJECTION"[3], pos));\n"
		"\n"
		"	// Only the first normal gets normalized (TODO: why?)\n"
		"	float3 _norm0 = float3(0.0, 0.0, 0.0);\n"
		"	if ((components & %uu) != 0u) // VB_HAS_NRM0\n", VB_HAS_NRM0);
	out.Write(
		"		_norm0 = normalize(float3(dot(N0, rawnorm0), dot(N1, rawnorm0), dot(N2, rawnorm0)));\n"
		"\n"
		"	float3 _norm1 = float3(0.0, 0.0, 0.0);\n"
		"	if ((components & %uu) != 0u) // VB_HAS_NRM1\n", VB_HAS_NRM1);
	out.Write(
		"		_norm1 = float3(dot(N0, rawnorm1), dot(N1, rawnorm1), dot(N2, rawnorm1));\n"
		"\n"
		"	float3 _norm2 = float3(0.0, 0.0, 0.0);\n"
		"	if ((components & %uu) != 0u) // VB_HAS_NRM2\n", VB_HAS_NRM2);
	out.Write(
		"		_norm2 = float3(dot(N0, rawnorm2), dot(N1, rawnorm2), dot(N2, rawnorm2));\n"
		"\n");

  // Hardware Lighting
  out.WriteLine("if ((components & %uu) != 0) // VB_HAS_COL0", VB_HAS_COL0);
  out.WriteLine("  o.colors_0 = color0;");
  out.WriteLine("else");
  out.WriteLine("  o.colors_0 = float4(1.0, 1.0, 1.0, 1.0);");
  out.WriteLine("");
  out.WriteLine("if ((components & %uu) != 0) // VB_HAS_COL1", VB_HAS_COL1);
  out.WriteLine("  o.colors_1 = color1;");
  out.WriteLine("else");
  out.WriteLine("  o.colors_1 = float4(1.0, 1.0, 1.0, 1.0);");
  out.WriteLine("");

  out.WriteLine("// Lighting");
  out.WriteLine("for (uint i = 0; i < xfmem_numColorChans; i++) {");
  out.WriteLine("  int4 mat = " I_MATERIALS "[i + 2u];");
  out.WriteLine("  int4 lacc = int4(255, 255, 255, 255);");
  out.WriteLine("");

  out.WriteLine("  if (%s != 0u) {", BitfieldExtract("xfmem_color[i]", LitChannel().matsource).c_str());
  out.WriteLine("    if ((components & (%uu << i)) != 0u) // VB_HAS_COL0", VB_HAS_COL0);
  out.WriteLine("      mat.xyz = int3(round(((i == 0u) ? color0.xyz : color1.xyz) * 255.0));");
  out.WriteLine("    else if ((components & %uu) != 0) // VB_HAS_COLO0", VB_HAS_COL0);
  out.WriteLine("      mat.xyz = int3(round(color0.xyz * 255.0));");
  out.WriteLine("    else");
  out.WriteLine("      mat.xyz = int3(255, 255, 255);");
  out.WriteLine("  }");
  out.WriteLine("");

  out.WriteLine("  if (%s != 0u) {", BitfieldExtract("xfmem_alpha[i]", LitChannel().matsource).c_str());
  out.WriteLine("    if ((components & (%uu << i)) != 0u) // VB_HAS_COL0", VB_HAS_COL0);
  out.WriteLine("      mat.w = int(round(((i == 0u) ? color0.w : color1.w) * 255.0));");
  out.WriteLine("    else if ((components & %uu) != 0) // VB_HAS_COLO0", VB_HAS_COL0);
  out.WriteLine("      mat.w = int(round(color0.w * 255.0));");
  out.WriteLine("    else");
  out.WriteLine("      mat.w = 255;");
  out.WriteLine("  } else {");
  out.WriteLine("    mat.w = " I_MATERIALS " [i + 2u].w;");
  out.WriteLine("  }");
  out.WriteLine("");

  out.WriteLine("  if (%s != 0u) {", BitfieldExtract("xfmem_color[i]", LitChannel().enablelighting).c_str());
  out.WriteLine("    if (%s != 0u) {", BitfieldExtract("xfmem_color[i]", LitChannel().ambsource).c_str());
  out.WriteLine("      if ((components & (%uu << i)) != 0u) // VB_HAS_COL0", VB_HAS_COL0);
  out.WriteLine("        lacc.xyz = int3(round(((i == 0u) ? color0.xyz : color1.xyz) * 255.0));");
  out.WriteLine("      else if ((components & %uu) != 0) // VB_HAS_COLO0", VB_HAS_COL0);
  out.WriteLine("        lacc.xyz = int3(round(color0.xyz * 255.0));");
  out.WriteLine("      else");
  out.WriteLine("        lacc.xyz = int3(255, 255, 255);");
  out.WriteLine("    } else {");
  out.WriteLine("      lacc.xyz = " I_MATERIALS " [i].xyz;");
  out.WriteLine("    }");
  out.WriteLine("");
  out.WriteLine("    uint light_mask = %s | (%s << 4u);", BitfieldExtract("xfmem_color[i]", LitChannel().lightMask0_3).c_str(), BitfieldExtract("xfmem_color[i]", LitChannel().lightMask4_7).c_str());
  out.WriteLine("    uint attnfunc = %s;", BitfieldExtract("xfmem_color[i]", LitChannel().attnfunc).c_str());
  out.WriteLine("    uint diffusefunc = %s;", BitfieldExtract("xfmem_color[i]", LitChannel().diffusefunc).c_str());
  out.WriteLine("    for (uint light_index = 0; light_index < 8u; light_index++) {");
  out.WriteLine("      if ((light_mask & (1u << light_index)) != 0)");
  out.WriteLine("        lacc.xyz += CalculateLighting(light_index, attnfunc, diffusefunc, pos, _norm0).xyz;");
  out.WriteLine("    }");
  out.WriteLine("  }");
  out.WriteLine("");

  out.WriteLine("  if (%s != 0u) {", BitfieldExtract("xfmem_alpha[i]", LitChannel().enablelighting).c_str());
  out.WriteLine("    if (%s != 0u) {", BitfieldExtract("xfmem_alpha[i]", LitChannel().ambsource).c_str());
  out.WriteLine("      if ((components & (%uu << i)) != 0u) // VB_HAS_COL0", VB_HAS_COL0);
  out.WriteLine("        lacc.w = int(round(((i == 0u) ? color0.w : color1.w) * 255.0));");
  out.WriteLine("      else if ((components & %uu) != 0) // VB_HAS_COLO0", VB_HAS_COL0);
  out.WriteLine("        lacc.w = int(round(color0.w * 255.0));");
  out.WriteLine("      else");
  out.WriteLine("        lacc.w = 255;");
  out.WriteLine("    } else {");
  out.WriteLine("      lacc.w = " I_MATERIALS " [i].w;");
  out.WriteLine("    }");
  out.WriteLine("");
  out.WriteLine("    uint light_mask = %s | (%s << 4u);", BitfieldExtract("xfmem_alpha[i]", LitChannel().lightMask0_3).c_str(), BitfieldExtract("xfmem_alpha[i]", LitChannel().lightMask4_7).c_str());
  out.WriteLine("    uint attnfunc = %s;", BitfieldExtract("xfmem_alpha[i]", LitChannel().attnfunc).c_str());
  out.WriteLine("    uint diffusefunc = %s;", BitfieldExtract("xfmem_alpha[i]", LitChannel().diffusefunc).c_str());
  out.WriteLine("    for (uint light_index = 0; light_index < 8u; light_index++) {");
  out.WriteLine("      if ((light_mask & (1u << light_index)) != 0)");
  out.WriteLine("        lacc.w += CalculateLighting(light_index, attnfunc, diffusefunc, pos, _norm0).w;");
  out.WriteLine("    }");
  out.WriteLine("  }");
  out.WriteLine("");

  out.WriteLine("  lacc = clamp(lacc, 0, 255);");
  out.WriteLine("");

  out.WriteLine("  // Hopefully GPUs that can support dynamic indexing will optimize this.");
  out.WriteLine("  float4 lit_color = float4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;");
  out.WriteLine("  switch (i) {");
  out.WriteLine("  case 0: o.colors_0 = lit_color; break;");
  out.WriteLine("  case 1: o.colors_1 = lit_color; break;");
  out.WriteLine("  }");
  out.WriteLine("}");
  out.WriteLine("");

  out.WriteLine("if (xfmem_numColorChans < 2u && (components & %uu) == 0)", VB_HAS_COL1);
  out.WriteLine("  o.colors_1 = o.colors_0;");

	// Texture Coordinates
  // TODO: Should we unroll this at compile time?
  if (numTexgen > 0)
  {
    // The HLSL compiler complains that the output texture coordinates are uninitialized when trying to dynamically index them.
    out.WriteLine("// Texture coordinate generation");
    for (u32 i = 0; i < numTexgen; i++)
      out.WriteLine("o.tex[%u] = float3(0.0, 0.0, 0.0);", i);
    out.WriteLine("");

    out.WriteLine("for (uint i = 0u; i < %uu; i++) {", numTexgen);
    out.WriteLine("  // Texcoord transforms");
    out.WriteLine("  float4 coord = float4(0.0, 0.0, 1.0, 1.0);");
    out.WriteLine("  switch (%s) {", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().sourcerow).c_str());
    out.WriteLine("  case %uu: // XF_SRCGEOM_INROW", XF_SRCGEOM_INROW);
    out.WriteLine("    coord.xyz = rawpos.xyz;");
    out.WriteLine("    break;");
    out.WriteLine("  case %uu: // XF_SRCNORMAL_INROW", XF_SRCNORMAL_INROW);
    out.WriteLine("    coord.xyz = ((components & %uu /* VB_HAS_NRM0 */) != 0u) ? rawnorm0.xyz : coord.xyz;", VB_HAS_NRM0);
    out.WriteLine("    break;");
    out.WriteLine("  case %uu: // XF_SRCBINORMAL_T_INROW", XF_SRCBINORMAL_T_INROW);
    out.WriteLine("    coord.xyz = ((components & %uu /* VB_HAS_NRM1 */) != 0u) ? rawnorm1.xyz : coord.xyz;", VB_HAS_NRM1);
    out.WriteLine("    break;");
    out.WriteLine("  case %uu: // XF_SRCBINORMAL_B_INROW", XF_SRCBINORMAL_B_INROW);
    out.WriteLine("    coord.xyz = ((components & %uu /* VB_HAS_NRM2 */) != 0u) ? rawnorm2.xyz : coord.xyz;", VB_HAS_NRM2);
    out.WriteLine("    break;");
    for (u32 i = 0; i < 8; i++)
    {
      out.WriteLine("  case %uu: // XF_SRCTEX%u_INROW", XF_SRCTEX0_INROW + i, i);
      out.WriteLine("    coord = ((components & %uu /* VB_HAS_UV%u */) != 0u) ? float4(tex%u.x, tex%u.y, 1.0, 1.0) : coord;",
                    VB_HAS_UV0 << i, i, i, i);
      out.WriteLine("    break;");
    }
    out.WriteLine("  }");
    out.WriteLine("");

    out.WriteLine("  // Input form of AB11 sets z element to 1.0");
    out.WriteLine("  if (%s == %uu) // inputform == XF_TEXINPUT_AB11", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().inputform).c_str(), XF_TEXINPUT_AB11);
    out.WriteLine("    coord.z = 1.0f;");
    out.WriteLine("");

    out.WriteLine("  // first transformation");
    out.WriteLine("  uint texgentype = %s;", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().texgentype).c_str());
    out.WriteLine("  switch (texgentype)");
    out.WriteLine("  {");
    out.WriteLine("  case %uu: // XF_TEXGEN_EMBOSS_MAP", XF_TEXGEN_EMBOSS_MAP);
    out.WriteLine("    {");
    out.WriteLine("      uint light = %s;", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().embosslightshift).c_str());
    out.WriteLine("      uint source = %s;", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().embosssourceshift).c_str());
    out.WriteLine("      if ((components & %uu) != 0) { // VB_HAS_NRM1 | VB_HAS_NRM2");   // Should this be == (NRM1 | NRM2)?
    out.WriteLine("        float3 ldir = normalize(" I_LIGHTS "[light].pos.xyz - pos.xyz);");
    out.WriteLine("        o.tex[i].xyz = o.tex[source].xyz + float3(dot(ldir, _norm1), dot(ldir, _norm2), 0.0);");
    out.WriteLine("      } else {");
    out.WriteLine("        o.tex[i].xyz = o.tex[source].xyz;");
    out.WriteLine("      }");
    out.WriteLine("    }");
    out.WriteLine("    break;");
    out.WriteLine("  case %uu: // XF_TEXGEN_COLOR_STRGBC0", XF_TEXGEN_COLOR_STRGBC0);
    out.WriteLine("    o.tex[i].xyz = float3(o.colors_0.x, o.colors_0.y, 1.0);");
    out.WriteLine("    break;");
    out.WriteLine("  case %uu: // XF_TEXGEN_COLOR_STRGBC1", XF_TEXGEN_COLOR_STRGBC1);
    out.WriteLine("    o.tex[i].xyz = float3(o.colors_1.x, o.colors_1.y, 1.0);");
    out.WriteLine("    break;");
    out.WriteLine("  default:  // Also XF_TEXGEN_REGULAR");
    out.WriteLine("    {");
    out.WriteLine("      if ((components & (%uu /* VB_HAS_TEXMTXIDX0 */ << i)) != 0) {", VB_HAS_TEXMTXIDX0);
    out.WriteLine("        // This is messy, due to dynamic indexing of the input texture coordinates.");
    out.WriteLine("        // Hopefully the compiler will unroll this whole loop anyway and the switch.");
    out.WriteLine("        int tmp = 0;");
    out.WriteLine("        switch (i) {");
    for (u32 i = 0; i < numTexgen; i++)
      out.WriteLine("        case %u: tmp = int(tex%u.z); break;", i, i);
    out.WriteLine("        }");
    out.WriteLine("");
    out.WriteLine("        if (%s == %uu) {", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().projection).c_str(), XF_TEXPROJ_STQ);
    out.WriteLine("          o.tex[i].xyz = float3(dot(coord, " I_TRANSFORMMATRICES "[tmp]),");
    out.WriteLine("                                dot(coord, " I_TRANSFORMMATRICES "[tmp + 1]),");
    out.WriteLine("                                dot(coord, " I_TRANSFORMMATRICES "[tmp + 2]));");
    out.WriteLine("        } else {");
    out.WriteLine("          o.tex[i].xyz = float3(dot(coord, " I_TRANSFORMMATRICES "[tmp]),");
    out.WriteLine("                                dot(coord, " I_TRANSFORMMATRICES "[tmp + 1]),");
    out.WriteLine("                                1.0);");
    out.WriteLine("        }");
    out.WriteLine("      } else {");
    out.WriteLine("        if (%s == %uu) {", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().projection).c_str(), XF_TEXPROJ_STQ);
    out.WriteLine("          o.tex[i].xyz = float3(dot(coord, " I_TEXMATRICES "[3 * i]),");
    out.WriteLine("                                dot(coord, " I_TEXMATRICES "[3 * i + 1]),");
    out.WriteLine("                                dot(coord, " I_TEXMATRICES "[3 * i + 2]));");
    out.WriteLine("        } else {");
    out.WriteLine("          o.tex[i].xyz = float3(dot(coord, " I_TEXMATRICES "[3 * i]),");
    out.WriteLine("                                dot(coord, " I_TEXMATRICES "[3 * i + 1]),");
    out.WriteLine("                                1.0);");
    out.WriteLine("        }");
    out.WriteLine("      }");
    out.WriteLine("    }");
    out.WriteLine("    break;");
    out.WriteLine("  }");
    out.WriteLine("");

    out.WriteLine("  if (xfmem_dualTexInfo != 0u) {");
    out.WriteLine("    uint base_index = %s;", BitfieldExtract("xfmem_postMtxInfo[i]", PostMtxInfo().index).c_str());
    out.WriteLine("    float4 P0 = " I_POSTTRANSFORMMATRICES "[base_index & 0x3fu];");
    out.WriteLine("    float4 P1 = " I_POSTTRANSFORMMATRICES "[(base_index + 1u) & 0x3fu];");
    out.WriteLine("    float4 P2 = " I_POSTTRANSFORMMATRICES "[(base_index + 2u) & 0x3fu];");
    out.WriteLine("");
    
    out.WriteLine("    if (%s != 0u)", BitfieldExtract("xfmem_postMtxInfo[i]", PostMtxInfo().normalize).c_str());
    out.WriteLine("      o.tex[i].xyz = normalize(o.tex[i].xyz);");
    out.WriteLine("");

    out.WriteLine("    // multiply by postmatrix");
    out.WriteLine("    o.tex[i].xyz = float3(dot(P0.xyz, o.tex[i].xyz) + P0.w,");
    out.WriteLine("                          dot(P1.xyz, o.tex[i].xyz) + P1.w,");
    out.WriteLine("                          dot(P2.xyz, o.tex[i].xyz) + P2.w);");
    out.WriteLine("  }");

    // When q is 0, the GameCube appears to have a special case
    // This can be seen in devkitPro's neheGX Lesson08 example for Wii
    // Makes differences in Rogue Squadron 3 (Hoth sky) and The Last Story (shadow culling)
    out.WriteLine("  if (texgentype == %uu && o.tex[i].z == 0.0) // XF_TEXGEN_REGULAR", XF_TEXGEN_REGULAR);
    out.WriteLine("    o.tex[i].xy = clamp(o.tex[i].xy / 2.0f, float2(-1.0f,-1.0f), float2(1.0f,1.0f));");
    out.WriteLine("}");
  }

  // clipPos/w needs to be done in pixel shader, not here
  out.Write("o.clipPos = o.pos;\n");

  // If we can disable the incorrect depth clipping planes using depth clamping, then we can do
  // our own depth clipping and calculate the depth range before the perspective divide if
  // necessary.
  if (g_ActiveConfig.backend_info.bSupportsDepthClamp)
  {
    // Since we're adjusting z for the depth range before the perspective divide, we have to do our
    // own clipping. We want to clip so that -w <= z <= 0, which matches the console -1..0 range.
    // We adjust our depth value for clipping purposes to match the perspective projection in the
    // software backend, which is a hack to fix Sonic Adventure and Unleashed games.
    out.Write("float clipDepth = o.pos.z * (1.0 - 1e-7);\n");
    out.Write("o.clipDist0 = clipDepth + o.pos.w;\n");  // Near: z < -w
    out.Write("o.clipDist1 = -clipDepth;\n");           // Far: z > 0
  }

  // Write the true depth value. If the game uses depth textures, then the pixel shader will
  // override it with the correct values if not then early z culling will improve speed.
  // There are two different ways to do this, when the depth range is oversized, we process
  // the depth range in the vertex shader, if not we let the host driver handle it.
  //
  // Adjust z for the depth range. We're using an equation which incorperates a depth inversion,
  // so we can map the console -1..0 range to the 0..1 range used in the depth buffer.
  // We have to handle the depth range in the vertex shader instead of after the perspective
  // divide, because some games will use a depth range larger than what is allowed by the
  // graphics API. These large depth ranges will still be clipped to the 0..1 range, so these
  // games effectively add a depth bias to the values written to the depth buffer.
  out.Write("o.pos.z = o.pos.w * " I_PIXELCENTERCORRECTION ".w - "
            "o.pos.z * " I_PIXELCENTERCORRECTION ".z;\n");

  if (!g_ActiveConfig.backend_info.bSupportsClipControl)
  {
    // If the graphics API doesn't support a depth range of 0..1, then we need to map z to
    // the -1..1 range. Unfortunately we have to use a substraction, which is a lossy floating-point
    // operation that can introduce a round-trip error.
    out.Write("o.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
  }

  // Correct for negative viewports by mirroring all vertices. We need to negate the height here,
  // since the viewport height is already negated by the render backend.
  out.Write("o.pos.xy *= sign(" I_PIXELCENTERCORRECTION ".xy * float2(1.0, -1.0));\n");

  // The console GPU places the pixel center at 7/12 in screen space unless
  // antialiasing is enabled, while D3D and OpenGL place it at 0.5. This results
  // in some primitives being placed one pixel too far to the bottom-right,
  // which in turn can be critical if it happens for clear quads.
  // Hence, we compensate for this pixel center difference so that primitives
  // get rasterized correctly.
  out.Write("o.pos.xy = o.pos.xy - o.pos.w * " I_PIXELCENTERCORRECTION ".xy;\n");

  if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
  {
    if (g_ActiveConfig.backend_info.bSupportsGeometryShaders || ApiType == APIType::Vulkan)
    {
      AssignVSOutputMembers(out, "vs", "o", numTexgen, false);
    }
    else
    {
      // TODO: Pass interface blocks between shader stages even if geometry shaders
      // are not supported, however that will require at least OpenGL 3.2 support.
      for (u32 i = 0; i < numTexgen; ++i)
        out.Write("uv%d.xyz = o.tex[%d];\n", i, i);
      out.Write("clipPos = o.clipPos;\n");
      out.Write("colors_0 = o.colors_0;\n");
      out.Write("colors_1 = o.colors_1;\n");
    }

    if (g_ActiveConfig.backend_info.bSupportsDepthClamp)
    {
      out.Write("gl_ClipDistance[0] = o.clipDist0;\n");
      out.Write("gl_ClipDistance[1] = o.clipDist1;\n");
    }

    // Vulkan NDC space has Y pointing down (right-handed NDC space).
    if (ApiType == APIType::Vulkan)
      out.Write("gl_Position = float4(o.pos.x, -o.pos.y, o.pos.z, o.pos.w);\n");
    else
      out.Write("gl_Position = o.pos;\n");
  }
  else  // D3D
  {
    out.Write("return o;\n");
  }
  out.Write("}\n");

	return out;
}


}