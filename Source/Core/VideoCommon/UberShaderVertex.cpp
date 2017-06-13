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

	// Lighting (basic color passthrough for now)
	out.Write(
		"	if ((components & %uu) != 0u) // VB_HAS_COL0\n", VB_HAS_COL0);
	out.Write(
		"		o.colors_0 = color0;\n"
		"	else\n"
		"		o.colors_0 = float4(1.0, 1.0, 1.0, 1.0);\n"
		"\n"
		"	if ((components & %uu) != 0u) // VB_HAS_COL1\n", VB_HAS_COL1);
	out.Write(
		"		o.colors_1 = color1;\n"
		"	else\n"
		"		o.colors_1 = o.colors_0;\n"
		"\n");

	// TODO: Actual Hardware Lighting

	// Texture Coordinates
  // TODO: Should we unroll this at compile time?
  if (numTexgen > 0)
  {
    out.WriteLine("// Texture coordinate generation");
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
    out.WriteLine("  if (%s != 0u)", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().inputform).c_str());
    out.WriteLine("    coord.z = 1.0f;");
    out.WriteLine("");

    out.WriteLine("  // first transformation");
    out.WriteLine("  switch (%s) {", BitfieldExtract("xfmem_texMtxInfo[i]", TexMtxInfo().texgentype).c_str());
    out.WriteLine("  case %uu: // XF_TEXGEN_EMBOSS_MAP", XF_TEXGEN_EMBOSS_MAP);
    out.WriteLine("    // TODO");
    out.WriteLine("    o.tex[i] = float3(0.0, 0.0, 0.0);");
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

    out.WriteLine("}");
  }

#if 0
	// Simple Texture coord code: each texcoordX is hardcoded to texgenX
	for (u32 i = 0; i < numTexgen; i++)
	{
		out.Write("	{\n");
		out.Write("		// This UV generation code is overly simplied, texCoord %i is not hardwired to texgen %i\n", i, i);
		out.Write("		float4 coord = float4(tex%d.x, tex%d.y, 1.0, 1.0);\n", i, i);
		out.Write("		o.tex[%d].xyz = float3(dot(coord, " I_TEXMATRICES"[%d]), dot(coord, " I_TEXMATRICES"[%d]), 1);\n", i, 3 * i, 3 * i + 1);
		out.Write("	}\n");
	}
#endif

  // TODO: Per Vertex Lighting?

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