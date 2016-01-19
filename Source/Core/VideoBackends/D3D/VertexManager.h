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
	void ResetBuffer(u32 stride, u32 min_vbuffer_space, u32 min_ibuffer_space) override;
	u16* GetIndexBuffer() { return &LocalIBuffer[0]; }

private:

	void PrepareDrawBuffers(u32 stride);
	void Draw(u32 stride);
	// temp
	void vFlush(bool useDstAlpha) override;

	ID3D11Buffer* m_vertex_buffer;
	ID3D11Buffer* m_index_buffer;

	u32 m_vertex_buffer_cursor;		// in bytes
	u32 m_index_buffer_cursor;		// in words

	u32 m_base_vertex;
	u32 m_base_index;

	std::vector<u8> LocalVBuffer;
	std::vector<u16> LocalIBuffer;
};

}  // namespace
