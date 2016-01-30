// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "VideoCommon/VertexManagerBase.h"

namespace DX11
{

class VertexManager : public VertexManagerBase
{
public:
	VertexManager();
	~VertexManager();

	NativeVertexFormat* CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
	void CreateDeviceObjects() override;
	void DestroyDeviceObjects() override;

protected:
	void ResetBuffer(u32 stride) override;
	u16* GetIndexBuffer() { return &LocalIBuffer[0]; }

private:

	void PrepareDrawBuffers(u32 stride);
	void Draw(u32 stride);
	// temp
	void vFlush(bool useDstAlpha) override;

	u32 m_vertexDrawOffset;
	u32 m_indexDrawOffset;
	u32 m_currentBuffer;
	u32 m_bufferCursor;

	enum { MAX_BUFFER_COUNT = 2 };
	ID3D11Buffer* m_buffers[MAX_BUFFER_COUNT];

	std::vector<u8> LocalVBuffer;
	std::vector<u16> LocalIBuffer;

public:
	struct CacheEntry : CacheEntryBase
	{
		~CacheEntry();

		//ID3D11Buffer* VertexAndIndexBuffer;
		ID3D11Buffer* VertexBuffer;
		ID3D11Buffer* IndexBuffer;
		u32 VertexCount;
		u32 VertexStride;
		u32 StartIndex;
		u32 IndexCount;
	};

	CacheEntryBase* CreateCacheEntry() override;
	void DeleteCacheEntry(CacheEntryBase* entry) override;
	void PopulateCacheEntry(CacheEntryBase* entry) override;
	void DrawCacheEntry(CacheEntryBase* entry, u32 indexCount, bool useDstAlpha) override;
};

}  // namespace
