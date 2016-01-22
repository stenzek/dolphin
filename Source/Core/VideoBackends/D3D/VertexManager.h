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

	static void Draw(PrimitiveType primitive, const u8* vertexData, u32 vertexDataSize, u32 vertexStride, const u16* indices, u32 indexCount);

protected:
	void ResetBuffer(u32 stride) override;
	u16* GetIndexBuffer() { return &LocalIBuffer[0]; }

private:

	void PrepareDrawBuffers(const u8* vertexData, u32 vertexDataSize, u32 vertexStride, const u16* indices, u32 indexCount);
	void Draw(PrimitiveType primitive, u32 vertexStride, u32 indexCount);

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
};

}  // namespace
