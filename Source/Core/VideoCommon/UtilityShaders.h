// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

static const char PASSTHROUGH_VERTEX_SHADER_SOURCE[] = R"(
#if API_D3D
struct VS_INPUT
{
  float4 pos : POSITION;
  float4 col0 : COLOR0;
  float3 tex0 : TEXCOORD0;
};
struct VS_OUTPUT
{
  float4 pos : SV_Position;
  float4 col0 : COLOR0;
  float3 tex0 : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT vin)
{
  VS_OUTPUT vout;
  vout.pos = vin.pos;
  vout.col0 = vin.col0;
  vout.tex0 = vin.tex0;
  return vout;
}

#elif API_OPENGL || API_VULKAN
ATTRIBUTE_LOCATION(0) in vec4 ipos;
ATTRIBUTE_LOCATION(5) in vec4 icol0;
ATTRIBUTE_LOCATION(8) in vec3 itex0;
VARYING_LOCATION(0) out vec4 ocol0;
VARYING_LOCATION(1) out vec3 otex0;

void main()
{
  gl_Position = ipos;
  ocol0 = icol0;
  otex0 = itex0;
}

#elif API_METAL
struct VS_INPUT
{
  float4 pos [[attribute(0)]];
  float4 col0 [[attribute(5)]];
  float3 tex0 [[attribute(8)]];
};
struct VS_OUTPUT
{
  float4 pos [[position]];
  float4 col0;
  float3 tex0;
};

vertex VS_OUTPUT vmain(VS_INPUT in [[stage_in]])
{
  VS_OUTPUT out;
  out.pos = in.pos;
  out.col0 = in.col0;
  out.tex0 = in.tex0;
  return out;
}

#endif
)";

static const char SCREEN_QUAD_VERTEX_SHADER_SOURCE[] = R"(
#if API_D3D
struct VS_OUTPUT
{
  float4 pos : SV_Position;
  float4 col0 : COLOR0;
  float3 tex0 : TEXCOORD0;
};

VS_OUTPUT main(uint vid : SV_VertexID)
{
  VS_OUTPUT vout;
  float2 rawpos = float2(float(vid & 1u), clamp(float(vid & 2u), 0.0, 1.0));
  vout.pos = float4(rawpos * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  vout.col0 = float4(1.0, 1.0, 1.0, 1.0);
  vout.tex0 = float3(rawpos, 0.0);
  return vout;
}

#elif API_OPENGL || API_VULKAN
VARYING_LOCATION(0) out vec4 ocol0;
VARYING_LOCATION(1) out vec3 otex0;

void main()
{
  float2 rawpos = float2(float(gl_VertexID & 1), clamp(float(gl_VertexID & 2), 0.0, 1.0));
#if API_OPENGL
  gl_Position = float4(rawpos * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
#else
  gl_Position = vec4(rawpos * 2.0f - 1.0f, 0.0f, 1.0f);
#endif
  ocol0 = float4(1.0, 1.0, 1.0, 1.0);
  otex0 = float3(rawpos, 0.0);
}

#elif API_METAL
struct VS_OUTPUT
{
 float4 pos [[position]];
 float4 col0;
 float3 uv0;
};

vertex VS_OUTPUT vmain(uint vid [[vertex_id]])
{
  VS_OUTPUT out;
  float2 rawpos = float2(float(vid & 1u), clamp(float(vid & 2u), 0.0f, 1.0f));
  out.pos = float4(rawpos * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0f, 1.0f);
  out.col0 = float4(1.0f, 1.0f, 1.0f, 1.0f);
  out.uv0 = float3(rawpos, 0.0f);
  return out;
}

#endif
)";

static const char STEREO_EXPAND_GEOMETRY_SHADER_SOURCE[] = R"(
#if API_D3D
struct VS_OUTPUT
{
  float4 pos : SV_Position;
  float4 col0 : COLOR0;
  float3 tex0 : TEXCOORD0;
};
struct GS_OUTPUT
{
  float4 pos : SV_Position;
  float4 col0 : COLOR0;
  float3 tex0 : TEXCOORD0;
  uint slice : SV_RenderTargetArrayIndex;
};

[maxvertexcount(6)]
void main(triangle VS_OUTPUT o[3], inout TriangleStream<GS_OUTPUT> output)
{
  for(int slice = 0; slice < 2; slice++)
  {
    for(int i = 0; i < 3; i++)
    {
      GS_OUTPUT gout;
      gout.pos = o[i].pos;
      gout.col0 = o[i].col0;
      gout.tex0 = float3(o[i].tex0.xy, float(slice));
      gout.slice = uint(slice);
      output.Append(gout);
    }
    output.RestartStrip();
  }
}

#elif API_OPENGL || API_VULKAN
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;

VARYING_LOCATION(0) in vec3 in_tex0[];
VARYING_LOCATION(1) in vec4 in_col0[];

VARYING_LOCATION(0) out vec3 out_tex0;
VARYING_LOCATION(1) out vec4 out_col0;

void main()
{
  for (int j = 0; j < 2; j++)
  {
    for (int i = 0; i < 3; i++)
    {
      gl_Layer = j;
      gl_Position = gl_in[i].gl_Position;
      out_tex0 = vec3(in_tex0[i].xy, float(j));
      out_col0 = in_col0[i];
      EmitVertex();
    }
    EndPrimitive();
  }
}

#endif
)";

static const char CLEAR_PIXEL_SHADER_SOURCE[] = R"(
#if API_D3D
struct VS_OUTPUT
{
  float4 pos : SV_Position;
  float4 col0 : COLOR0;
  float3 tex0 : TEXCOORD0;
};

float4 main(VS_OUTPUT pin) : SV_Target
{
  return pin.col0;
}

#elif API_OPENGL || API_VULKAN
VARYING_LOCATION(0) in float3 tex0;
VARYING_LOCATION(1) in float4 col0;
FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;

void main()
{
  ocol0 = col0;
}

#elif API_METAL
struct VS_OUTPUT
{
  float4 pos [[position]];
  float4 col0;
  float3 tex0;
};

fragment float4 pmain(VS_OUTPUT in [[stage_in]])
{
  return in.col0;
}

#endif
)";

static const char BLIT_PIXEL_SHADER_SOURCE[] = R"(
#if API_D3D
struct VS_OUTPUT
{
  float4 pos : SV_Position;
  float4 col0 : COLOR0;
  float3 tex0 : TEXCOORD0;
};

Texture2DArray tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 main(VS_OUTPUT pin) : SV_Target
{
  return tex0.Sample(samp0, pin.tex0);
}

#elif API_OPENGL || API_VULKAN
VARYING_LOCATION(0) in float3 tex0;
VARYING_LOCATION(1) in float4 col0;
FRAGMENT_OUTPUT_LOCATION(0) out float4 ocol0;
SAMPLER_BINDING(0) uniform sampler2DArray samp0;

void main()
{
  ocol0 = texture(samp0, tex0);
}

#elif API_METAL
struct VS_OUTPUT
{
  float4 pos [[position]];
  float4 col0;
  float3 tex0;
};

fragment float4 pmain(VS_OUTPUT pin [[stage_in]],
                      texture2d_array<float> tex0 [[texture(0)]],
                      sampler samp0 [[sampler(0)]])
{
  return tex0.sample(samp0, pin.tex0.xy, int(pin.tex0.z));
}

#endif
)";
