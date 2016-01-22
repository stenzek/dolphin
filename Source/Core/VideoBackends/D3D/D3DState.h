// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <stack>
#include <unordered_map>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/BPMemory.h"

struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;

namespace DX11
{

union RasterizerState
{
	BitField<0, 2, D3D11_CULL_MODE> cull_mode;

	u32 packed;
};

union BlendState
{
	BitField<0, 1, u32> blend_enable;
	BitField<1, 3, D3D11_BLEND_OP> blend_op;
	BitField<4, 4, u32> write_mask;
	BitField<8, 5, D3D11_BLEND> src_blend;
	BitField<13, 5, D3D11_BLEND> dst_blend;
	BitField<18, 1, u32> use_dst_alpha;

	u32 packed;
};

union SamplerState
{
	BitField<0, 3, u64> min_filter;
	BitField<3, 1, u64> mag_filter;
	BitField<4, 8, u64> min_lod;
	BitField<12, 8, u64> max_lod;
	BitField<20, 8, s64> lod_bias;
	BitField<28, 2, u64> wrap_s;
	BitField<30, 2, u64> wrap_t;
	BitField<32, 5, u64> max_anisotropy;

	u64 packed;
};

class StateCache
{
public:

	// Get existing or create new render state.
	// Returned objects is owned by the cache and does not need to be released.
	ID3D11SamplerState* Get(SamplerState state);
	ID3D11BlendState* Get(BlendState state);
	ID3D11RasterizerState* Get(RasterizerState state);
	ID3D11DepthStencilState* Get(ZMode state);

	// Release all cached states and clear hash tables.
	void Clear();

private:

	std::unordered_map<u32, ID3D11DepthStencilState*> m_depth;
	std::unordered_map<u32, ID3D11RasterizerState*> m_raster;
	std::unordered_map<u32, ID3D11BlendState*> m_blend;
	std::unordered_map<u64, ID3D11SamplerState*> m_sampler;
};

namespace D3D
{

class StateManager
{
public:
	StateManager();

	void SetBlendState(ID3D11BlendState* state)
	{
		if (m_current.blendState != state)
			m_dirtyFlags |= DirtyFlag_BlendState;

		m_pending.blendState = state;
	}

	void SetDepthState(ID3D11DepthStencilState* state)
	{
		if (m_current.depthState != state)
			m_dirtyFlags |= DirtyFlag_DepthState;

		m_pending.depthState = state;
	}

	void SetRasterizerState(ID3D11RasterizerState* state)
	{
		if (m_current.rasterizerState != state)
			m_dirtyFlags |= DirtyFlag_RasterizerState;
		
		m_pending.rasterizerState = state;
	}

	void SetTexture(u32 index, ID3D11ShaderResourceView* texture)
	{
		if (m_current.textures[index] != texture)
			m_dirtyFlags |= DirtyFlag_Texture0 << index;

		m_pending.textures[index] = texture;
	}

	void SetSampler(u32 index, ID3D11SamplerState* sampler)
	{
		if (m_current.samplers[index] != sampler)
			m_dirtyFlags |= DirtyFlag_Sampler0 << index;

		m_pending.samplers[index] = sampler;
	}

	void SetPixelConstants(ID3D11Buffer* buffer0, ID3D11Buffer* buffer1 = nullptr)
	{
		if (m_current.pixelConstants[0] != buffer0 || m_current.pixelConstants[1] != buffer1)
			m_dirtyFlags |= DirtyFlag_PixelConstants;

		m_pending.pixelConstants[0] = buffer0;
		m_pending.pixelConstants[1] = buffer1;
	}

	void SetVertexConstants(ID3D11Buffer* buffer)
	{
		if (m_current.vertexConstants != buffer)
			m_dirtyFlags |= DirtyFlag_VertexConstants;

		m_pending.vertexConstants = buffer;
	}

	void SetGeometryConstants(ID3D11Buffer* buffer)
	{
		if (m_current.geometryConstants != buffer)
			m_pending.geometryConstants = buffer;

		m_dirtyFlags |= DirtyFlag_GeometryConstants;
	}

	void SetVertexBuffer(ID3D11Buffer* buffer, u32 stride, u32 offset)
	{
		if (m_current.vertexBuffer != buffer ||
			m_current.vertexBufferStride != stride ||
			m_current.vertexBufferOffset != offset)
		{
			m_dirtyFlags |= DirtyFlag_VertexBuffer;
		}

		m_pending.vertexBuffer = buffer;
		m_pending.vertexBufferStride = stride;
		m_pending.vertexBufferOffset = offset;
	}

	void SetIndexBuffer(ID3D11Buffer* buffer)
	{
		if (m_current.indexBuffer != buffer)
			m_dirtyFlags |= DirtyFlag_IndexBuffer;

		m_pending.indexBuffer = buffer;
	}

	void SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology)
	{
		if (m_current.topology != topology)
			m_dirtyFlags |= DirtyFlag_InputAssembler;

		m_pending.topology = topology;
	}

	void SetInputLayout(ID3D11InputLayout* layout)
	{
		if (m_current.inputLayout != layout)
			m_dirtyFlags |= DirtyFlag_InputAssembler;
		
		m_pending.inputLayout = layout;
	}

	void SetPixelShader(ID3D11PixelShader* shader)
	{
		if (m_current.pixelShader != shader)
			m_dirtyFlags |= DirtyFlag_PixelShader;

		m_pending.pixelShader = shader;
	}

	void SetVertexShader(ID3D11VertexShader* shader)
	{
		if (m_current.vertexShader != shader)
			m_dirtyFlags |= DirtyFlag_VertexShader;

		m_pending.vertexShader = shader;
	}

	void SetGeometryShader(ID3D11GeometryShader* shader)
	{
		if (m_current.geometryShader != shader)
			m_dirtyFlags |= DirtyFlag_GeometryShader;

		m_pending.geometryShader = shader;
	}

	void SetViewport(const D3D11_VIEWPORT &viewport)
	{
		if (memcmp(&m_current.viewport, &viewport, sizeof(D3D11_VIEWPORT)))
			m_dirtyFlags |= DirtyFlag_Viewport;

		memcpy(&m_pending.viewport, &viewport, sizeof(D3D11_VIEWPORT));
	}

	void SetScissorRect(const D3D11_RECT &rect)
	{
		if (memcmp(&m_current.scissorRect, &rect, sizeof(D3D11_RECT)))
			m_dirtyFlags |= DirtyFlag_ScissorRect;

		memcpy(&m_pending.scissorRect, &rect, sizeof(D3D11_RECT));
	}

	void SetRenderTarget(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv)
	{
		u32 count = (rtv) ? 1 : 0;
		if (m_current.renderTargetCount != count || m_current.renderTargets[0] != rtv || m_current.depthBuffer != dsv)
			m_dirtyFlags |= DirtyFlag_RenderTargets;

		m_pending.renderTargets[0] = rtv;
		for (u32 i = 1; i < 8; i++)
			m_pending.renderTargets[i] = nullptr;

		m_pending.depthBuffer = dsv;
		m_pending.renderTargetCount = count;
	}

	void SetRenderTargets(u32 count, ID3D11RenderTargetView** rtvs, ID3D11DepthStencilView* dsv)
	{
		if (m_current.renderTargetCount != count ||
			memcmp(m_current.renderTargets, rtvs, sizeof(ID3D11RenderTargetView*) * count) ||
			m_current.depthBuffer != dsv)
		{
			m_dirtyFlags |= DirtyFlag_RenderTargets;
		}

		for (u32 i = 0; i < count; i++)
			m_pending.renderTargets[i] = rtvs[i];
		for (u32 i = count; i < 8; i++)
			m_pending.renderTargets[i] = nullptr;

		m_pending.depthBuffer = dsv;
		m_pending.renderTargetCount = count;
	}

	// removes currently set texture from all slots, returns mask of previously bound slots
	u32 UnsetTexture(ID3D11ShaderResourceView* srv);
	void SetTextureByMask(u32 textureSlotMask, ID3D11ShaderResourceView* srv);

	// call this immediately before any drawing operation or to explicitly apply pending resource state changes
	void Apply(bool reapply = false);

private:

	enum DirtyFlags
	{
		DirtyFlag_Texture0 = 1 << 0,
		DirtyFlag_Texture1 = 1 << 1,
		DirtyFlag_Texture2 = 1 << 2,
		DirtyFlag_Texture3 = 1 << 3,
		DirtyFlag_Texture4 = 1 << 4,
		DirtyFlag_Texture5 = 1 << 5,
		DirtyFlag_Texture6 = 1 << 6,
		DirtyFlag_Texture7 = 1 << 7,

		DirtyFlag_Sampler0 = 1 << 8,
		DirtyFlag_Sampler1 = 1 << 9,
		DirtyFlag_Sampler2 = 1 << 10,
		DirtyFlag_Sampler3 = 1 << 11,
		DirtyFlag_Sampler4 = 1 << 12,
		DirtyFlag_Sampler5 = 1 << 13,
		DirtyFlag_Sampler6 = 1 << 14,
		DirtyFlag_Sampler7 = 1 << 15,

		DirtyFlag_PixelConstants = 1 << 16,
		DirtyFlag_VertexConstants = 1 << 17,
		DirtyFlag_GeometryConstants = 1 << 18,

		DirtyFlag_VertexBuffer = 1 << 19,
		DirtyFlag_IndexBuffer = 1 << 20,

		DirtyFlag_PixelShader = 1 << 21,
		DirtyFlag_VertexShader = 1 << 22,
		DirtyFlag_GeometryShader = 1 << 23,

		DirtyFlag_InputAssembler = 1 << 24,

		DirtyFlag_RasterizerState = 1 << 25,
		DirtyFlag_DepthState = 1 << 26,
		DirtyFlag_BlendState = 1 << 27,

		DirtyFlag_Viewport = 1 << 28,
		DirtyFlag_ScissorRect = 1 << 29,

		DirtyFlag_RenderTargets = 1 << 30
	};

	u32 m_dirtyFlags;

	struct Resources
	{
		ID3D11ShaderResourceView* textures[8];
		ID3D11SamplerState* samplers[8];
		ID3D11Buffer* pixelConstants[2];
		ID3D11Buffer* vertexConstants;
		ID3D11Buffer* geometryConstants;
		ID3D11Buffer* vertexBuffer;
		ID3D11Buffer* indexBuffer;
		u32 vertexBufferStride;
		u32 vertexBufferOffset;
		D3D11_PRIMITIVE_TOPOLOGY topology;
		ID3D11InputLayout* inputLayout;
		ID3D11PixelShader* pixelShader;
		ID3D11VertexShader* vertexShader;
		ID3D11GeometryShader* geometryShader;
		ID3D11BlendState* blendState;
		ID3D11DepthStencilState* depthState;
		ID3D11RasterizerState* rasterizerState;
		D3D11_VIEWPORT viewport;
		D3D11_RECT scissorRect;

		ID3D11RenderTargetView* renderTargets[8];
		ID3D11DepthStencilView* depthBuffer;
		u32 renderTargetCount;
	};

	Resources m_pending;
	Resources m_current;
};

extern StateManager* stateman;

}  // namespace

}  // namespace DX11
