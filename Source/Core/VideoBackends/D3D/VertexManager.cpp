// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/CommonTypes.h"

#include "VideoBackends/D3D/BoundingBox.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/GeometryShaderCache.h"
#include "VideoBackends/D3D/PixelShaderCache.h"
#include "VideoBackends/D3D/Render.h"
#include "VideoBackends/D3D/VertexManager.h"
#include "VideoBackends/D3D/VertexShaderCache.h"

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/Debugger.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{

void VertexManager::CreateDeviceObjects()
{
	D3D11_BUFFER_DESC vbufdesc = CD3D11_BUFFER_DESC(MAXVBUFFERSIZE,
		D3D11_BIND_INDEX_BUFFER | D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);

	D3D11_BUFFER_DESC ibufdesc = CD3D11_BUFFER_DESC(MAXIBUFFERSIZE * sizeof(u16),
		D3D11_BIND_INDEX_BUFFER | D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);

	CHECK(SUCCEEDED(D3D::device->CreateBuffer(&vbufdesc, nullptr, &m_vertex_buffer)), "Failed to create buffer.");
	CHECK(SUCCEEDED(D3D::device->CreateBuffer(&ibufdesc, nullptr, &m_index_buffer)), "Failed to create buffer.");
	D3D::SetDebugObjectName((ID3D11DeviceChild*)m_vertex_buffer, "Vertex Buffer of VertexManager");
	D3D::SetDebugObjectName((ID3D11DeviceChild*)m_index_buffer, "Index Buffer of VertexManager");

	m_vertex_buffer_cursor = 0;
	m_index_buffer_cursor = 0;

	m_base_vertex = 0;
	m_base_index = 0;
}

void VertexManager::DestroyDeviceObjects()
{
	SAFE_RELEASE(m_vertex_buffer);
	SAFE_RELEASE(m_index_buffer);
}

VertexManager::VertexManager()
{
	LocalVBuffer.resize(MAXVBUFFERSIZE);
	LocalIBuffer.resize(MAXIBUFFERSIZE);

	CreateDeviceObjects();
}

VertexManager::~VertexManager()
{
	DestroyDeviceObjects();
}

void VertexManager::PrepareDrawBuffers(u32 stride)
{
	u32 vertexBufferSize = u32(s_pCurBufferPointer - s_pBaseBufferPointer);
	u32 indexBufferSize = IndexGenerator::GetIndexLen() - m_index_buffer_cursor;

	D3D::context->Unmap(m_index_buffer, 0);
	D3D::context->Unmap(m_vertex_buffer, 0);

	m_base_vertex = m_vertex_buffer_cursor / stride;
	m_base_index = m_index_buffer_cursor;

	m_vertex_buffer_cursor += vertexBufferSize;
	m_index_buffer_cursor += indexBufferSize;

	_assert_(m_vertex_buffer_cursor <= MAXVBUFFERSIZE);
	_assert_(m_index_buffer_cursor <= MAXIBUFFERSIZE);

	ADDSTAT(stats.thisFrame.bytesVertexStreamed, vertexBufferSize);
	ADDSTAT(stats.thisFrame.bytesIndexStreamed, indexBufferSize);
}

void VertexManager::Draw(u32 stride)
{
	u32 indices = IndexGenerator::GetIndexLen() - m_base_index;

	D3D::stateman->SetVertexBuffer(m_vertex_buffer, stride, 0);
	D3D::stateman->SetIndexBuffer(m_index_buffer);

	switch (current_primitive_type)
	{
		case PRIMITIVE_POINTS:
			D3D::stateman->SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
			static_cast<Renderer*>(g_renderer.get())->ApplyCullDisable();
			break;
		case PRIMITIVE_LINES:
			D3D::stateman->SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
			static_cast<Renderer*>(g_renderer.get())->ApplyCullDisable();
			break;
		case PRIMITIVE_TRIANGLES:
			D3D::stateman->SetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			break;
	}

	D3D::stateman->Apply();
	D3D::context->DrawIndexed(indices, m_base_index, m_base_vertex);

	INCSTAT(stats.thisFrame.numDrawCalls);

	if (current_primitive_type != PRIMITIVE_TRIANGLES)
		static_cast<Renderer*>(g_renderer.get())->RestoreCull();
}

void VertexManager::vFlush(bool useDstAlpha)
{
	if (!PixelShaderCache::SetShader(
		useDstAlpha ? DSTALPHA_DUAL_SOURCE_BLEND : DSTALPHA_NONE))
	{
		GFX_DEBUGGER_PAUSE_LOG_AT(NEXT_ERROR,true,{printf("Fail to set pixel shader\n");});
		return;
	}

	if (!VertexShaderCache::SetShader())
	{
		GFX_DEBUGGER_PAUSE_LOG_AT(NEXT_ERROR,true,{printf("Fail to set pixel shader\n");});
		return;
	}

	if (!GeometryShaderCache::SetShader(current_primitive_type))
	{
		GFX_DEBUGGER_PAUSE_LOG_AT(NEXT_ERROR, true, { printf("Fail to set pixel shader\n"); });
		return;
	}

	if (g_ActiveConfig.backend_info.bSupportsBBox && BoundingBox::active)
	{
		D3D::context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 2, 1, &BBox::GetUAV(), nullptr);
	}

	u32 stride = VertexLoaderManager::GetCurrentVertexFormat()->GetVertexStride();

	PrepareDrawBuffers(stride);

	VertexLoaderManager::GetCurrentVertexFormat()->SetupVertexPointers();
	g_renderer->ApplyState(useDstAlpha);

	Draw(stride);

	g_renderer->RestoreState();
}

void VertexManager::ResetBuffer(u32 stride, u32 min_vbuffer_space, u32 min_ibuffer_space)
{
	if (s_cull_all)
	{
		// This buffer isn't getting sent to the GPU. Just allocate it on the cpu.
		s_pCurBufferPointer = s_pBaseBufferPointer = LocalVBuffer.data();
		s_pEndBufferPointer = s_pBaseBufferPointer + LocalIBuffer.size();
		IndexGenerator::Start((u16*)LocalIBuffer.data());
	}
	else
	{
		// Space without wrap-around?
		u32 cursor = m_vertex_buffer_cursor + ((m_vertex_buffer_cursor > 0) ? (stride - (m_vertex_buffer_cursor % stride)) : 0);
		u32 current_space = (cursor <= MAXVBUFFERSIZE) ? MAXVBUFFERSIZE - cursor : 0;
		D3D11_MAPPED_SUBRESOURCE map;
		if (current_space < min_vbuffer_space)
		{
			// Investigate do not wait here?
			HRESULT hr = D3D::context->Map(m_vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
			CHECK(SUCCEEDED(hr), "Map succeeded");
			cursor = 0;
		}
		else
		{
			HRESULT hr = D3D::context->Map(m_vertex_buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &map);
			CHECK(SUCCEEDED(hr), "Map succeeded");
		}

		m_vertex_buffer_cursor = cursor;
		s_pBaseBufferPointer = reinterpret_cast<u8*>(map.pData) + m_vertex_buffer_cursor;
		s_pCurBufferPointer = reinterpret_cast<u8*>(map.pData) + m_vertex_buffer_cursor;
		s_pEndBufferPointer = reinterpret_cast<u8*>(map.pData) + MAXVBUFFERSIZE;

		// Index buffer
		current_space = MAXIBUFFERSIZE - m_index_buffer_cursor;
		if (current_space < min_ibuffer_space)
		{
			HRESULT hr = D3D::context->Map(m_index_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
			CHECK(SUCCEEDED(hr), "Map succeeded");
			m_index_buffer_cursor = 0;
		}
		else
		{
			HRESULT hr = D3D::context->Map(m_index_buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &map);
			CHECK(SUCCEEDED(hr), "Map succeeded");
		}

		IndexGenerator::Resume(reinterpret_cast<u16*>(map.pData), reinterpret_cast<u16*>(map.pData) + m_index_buffer_cursor);
	}
}

}  // namespace
