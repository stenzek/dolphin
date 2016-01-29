#pragma once

#include <array>
#include <atomic>
#include <d3d11.h>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <wrl.h>
#include <vector>

#include "Common/BlockingLoop.h"
#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/TextureCache.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VertexManagerBase.h"

namespace DX11 {

class CommandStream
{
public:
	enum Command
	{
		CmdInvalid,
		CmdUploadTexture,
		CmdCopyEFBToTexture,
		CmdCopyEFBToRam,
		CmdLoadVertexConstants,
		CmdLoadGeometryConstants,
		CmdLoadPixelConstants,
		CmdLoadViewport,
		CmdLoadScissorRect,
		CmdClearScreen,
		CmdSetPipeline,
		CmdBindTexture,
		CmdSetSampler,
		CmdDraw,
		CmdSwap
	};

	struct BaseData
	{
		BaseData(Command type) : Type(type) {}
		virtual ~BaseData() {}

		u32 Size;
		Command Type;
	};

	struct UploadTextureData : BaseData
	{
		UploadTextureData() : BaseData(CmdUploadTexture) {}

		u32 Width;
		u32 Height;
		u32 RowPitch;
		u32 Level;
		TextureCache::TCacheEntry* Entry;
		u8* Data;
	};

	struct CopyEFBToTextureData : BaseData
	{
		CopyEFBToTextureData() : BaseData(CmdCopyEFBToTexture) {}

		ID3D11ShaderResourceView* DstTextureSRV;
		ID3D11RenderTargetView* DstTextureRTV;

		u32 TextureWidth;
		u32 TextureHeight;

		PEControl::PixelFormat SrcFormat;
		EFBRectangle SrcRect;
		bool ScaleByHalf;

		std::array<float, 28> ColMat;
		u32 ColMatIdx;
	};

	struct LoadVertexConstantsData : BaseData
	{
		LoadVertexConstantsData() : BaseData(CmdLoadVertexConstants) {}

		VertexShaderConstants Data;
	};

	struct LoadGeometryConstantsData : BaseData
	{
		LoadGeometryConstantsData() : BaseData(CmdLoadGeometryConstants) {}

		GeometryShaderConstants Data;
	};

	struct LoadPixelConstantsData : BaseData
	{
		LoadPixelConstantsData() : BaseData(CmdLoadPixelConstants) {}

		PixelShaderConstants Data;
	};

	struct LoadViewportData : BaseData
	{
		LoadViewportData() : BaseData(CmdLoadViewport) {}

		D3D11_VIEWPORT Viewport;
	};

	struct LoadScissorRectData : BaseData
	{
		LoadScissorRectData() : BaseData(CmdLoadScissorRect) {}

		D3D11_RECT ScissorRect;
	};

	struct ClearScreenData : BaseData
	{
		ClearScreenData() : BaseData(CmdClearScreen) {}

		EFBRectangle Rect;
		u32 ColorValue;
		u32 ZValue;
		bool ColorEnable;
		bool AlphaEnable;
		bool ZEnable;
	};

	struct SetPipelineData : BaseData
	{
		SetPipelineData() : BaseData(CmdSetPipeline) {}

		RasterizerState RState;
		ZMode DState;
		BlendState BState;
		ID3D11InputLayout* InputLayout;
		ID3D11VertexShader* VertexShader;
		ID3D11GeometryShader* GeometryShader;
		ID3D11PixelShader* PixelShader;
	};

	struct BindTextureData : BaseData
	{
		BindTextureData() : BaseData(CmdBindTexture) {}

		u32 Index;
		ID3D11ShaderResourceView* Texture;
	};

	struct SetSamplerData : BaseData
	{
		SetSamplerData() : BaseData(CmdSetSampler) {}

		u32 Index;
		SamplerState State;
	};

	struct DrawData : BaseData
	{
		DrawData() : BaseData(CmdDraw) {}

		PrimitiveType Primitive;
		u32 VertexDataSize;
		u32 VertexStride;
		u32 IndexCount;
		u8* VertexData;
		u16* IndexData;
	};

	struct SwapData : BaseData
	{
		SwapData() : BaseData(CmdSwap) {}

		u32 XFBAddr;
		u32 FBWidth;
		u32 FBStride;
		u32 FBHeight;
		EFBRectangle EFBRect;
		float Gamma;
	};

public:
	CommandStream();
	~CommandStream();

	// Initializer
	static bool Setup(bool use_worker_thread);
	static void Shutdown();
	static void Cleanup();

	// Frontend
	void UploadTexture(TextureCache::TCacheEntry* entry, u32 width, u32 height, u32 row_pitch, u32 level, const u8* data);
	void CopyEFBToTexture(TextureCache::TCacheEntry* entry, PEControl::PixelFormat srcFormat, const EFBRectangle &srcRect, bool scaleByHalf, const float* colmat, u32 colmatidx);
	void LoadVertexConstants(const VertexShaderConstants* data);
	void LoadGeometryConstants(const GeometryShaderConstants* data);
	void LoadPixelConstants(const PixelShaderConstants* data);
	void LoadViewport(const D3D11_VIEWPORT* viewport);
	void LoadScissorRect(const D3D11_RECT* data);
	void ClearScreen(const EFBRectangle& rc, bool colorEnable, bool alphaEnable, bool zEnable, u32 color, u32 z);
	void BindTexture(u32 index, ID3D11ShaderResourceView* texture);
	void SetSampler(u32 index, const SamplerState* state);
	void SetPipeline(const RasterizerState* rstate, const ZMode* dstate, const BlendState* bstate, ID3D11InputLayout* input_layout, ID3D11VertexShader* vertex_shader, ID3D11GeometryShader* geometry_shader, ID3D11PixelShader* pixel_shader);
	void Draw(PrimitiveType primitive, const u8* vertex_data, u32 vertex_data_size, u32 vertex_stride, const u16* index_data, u32 index_count);
	void Swap(u32 xfbAddr, u32 fbWidth, u32 fbStride, u32 fbHeight, const EFBRectangle& rc, float gamma);

	// Backend
	void BackendThreadLoop();
	void ResetAPIState();
	void RestoreAPIState();

private:
	// Initialization
	void CreateStateObjects();
	void DeleteStateObjects();

	// Command allocator
	template<class T> T* AllocateCommand(u32 required_aux_space = 0);

	// Aux buffer allocator for command
	void* AllocateCommandAux(BaseData* cmd, u32 count);

	// Command enqueue
	void EnqueueCommand(BaseData* cmd);
	
	// Command dequeue
	bool DequeueCommand(BaseData** cmd);

	// Command deallocator
	void DeallocateCommand(BaseData* cmd);

	// Buffer reset
	void ResetBuffer();

	// Backend
	void ExecCommand(BaseData* cmd);
	void ExecUploadTexture(const UploadTextureData* data);
	void ExecCopyEFBToTexture(const CopyEFBToTextureData* data);
	void ExecLoadVertexConstants(const LoadVertexConstantsData* data);
	void ExecLoadGeometryConstants(const LoadGeometryConstantsData* data);
	void ExecLoadPixelConstants(const LoadPixelConstantsData* data);
	void ExecLoadViewport(const LoadViewportData* data);
	void ExecLoadScissorRect(const LoadScissorRectData* data);
	void ExecClearScreen(const ClearScreenData* data);
	void ExecBindTexture(const BindTextureData* data);
	void ExecSetSampler(const SetSamplerData* data);
	void ExecSetPipeline(const SetPipelineData* data);
	void ExecDraw(const DrawData* data);
	void ExecSwap(const SwapData* data);

	// Common state objects, created at start time
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_default_rasterizer_state;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_default_depth_state;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_default_blend_state;

	// State objects for ClearScreen
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_clear_depth_state_write_enabled;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_clear_depth_state_write_disabled;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_clear_color_blend_state;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_clear_alpha_blend_state;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_clear_depth_blend_state;

	// Avoids state lookups when new pipeline state is loaded
	RasterizerState m_current_rasterizer_state;
	ZMode m_current_depth_state;
	BlendState m_current_blend_state;

	// List of current states for use in ResetAPIState/RestoreAPIState
	// TODO: Textures/samplers
	Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_save_rasterizer_state;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_save_depth_state;
	Microsoft::WRL::ComPtr<ID3D11BlendState> m_save_blend_state;
	Microsoft::WRL::ComPtr<ID3D11InputLayout> m_save_input_layout;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> m_save_vertex_shader;
	Microsoft::WRL::ComPtr<ID3D11GeometryShader> m_save_geometry_shader;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> m_save_pixel_shader;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_save_textures[8];
	Microsoft::WRL::ComPtr<ID3D11SamplerState> m_save_samplers[8];
	D3D11_VIEWPORT m_save_viewport;
	D3D11_RECT m_save_scissor_rect;

	// Command buffer
	// TODO: Replace with a lock-free ring buffer
	static const u32 COMMAND_ALLOCATION_ALIGNMENT = sizeof(void*);
	static const u32 COMMAND_BUFFER_SIZE = 32 * 1024 * 1024;
	u8* m_command_buffer;
	u32 m_command_buffer_rpos;
	u32 m_command_buffer_wpos;
	bool m_use_worker_thread;

	// Worker thread
	Common::BlockingLoop m_worker_control;

	// Last objects are owned by CPU thread, so split cache lines
	u8 __pad0[64];

	// Cache the last state on the decoding thread, that way we don't need to queue redundant commands.
	// If TextureCache bound stuff changed, this wouldn't be needed.
	// TODO: Pipeline state here
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_last_textures[8];
};

extern std::unique_ptr<CommandStream> g_command_stream;

}		// namespace DX11
