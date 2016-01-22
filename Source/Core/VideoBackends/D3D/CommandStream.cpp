#include "VideoBackends/D3D/CommandStream.h"
#include "VideoBackends/D3D/GeometryShaderCache.h"
#include "VideoBackends/D3D/PixelShaderCache.h"
#include "VideoBackends/D3D/VertexShaderCache.h"
#include "VideoBackends/D3D/VertexManager.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DUtil.h"
#include "VideoBackends/D3D/Render.h"
#include "VideoBackends/D3D/FramebufferManager.h"
#include "VideoCommon/Statistics.h"

namespace DX11 {

std::unique_ptr<CommandStream> g_command_stream;
std::unique_ptr<StateCache> g_state_cache;

CommandStream::CommandStream()
	: m_command_buffer(nullptr)
	, m_command_buffer_rpos(0)
	, m_command_buffer_wpos(0)
	, m_use_worker_thread(false)
{

}

CommandStream::~CommandStream()
{
	delete[] m_command_buffer;
}

bool CommandStream::Setup(bool use_worker_thread)
{
	_assert_(!g_command_stream);

	g_state_cache = std::make_unique<StateCache>();

	g_command_stream = std::make_unique<CommandStream>();
	g_command_stream->CreateStateObjects();
	
	g_command_stream->m_command_buffer = new u8[COMMAND_BUFFER_SIZE];
	g_command_stream->m_use_worker_thread = use_worker_thread;

	return true;
}

void CommandStream::Shutdown()
{
	if (g_command_stream->m_use_worker_thread)
		g_command_stream->m_worker_control.Stop(true);
}

void CommandStream::Cleanup()
{
	g_command_stream.reset();
	g_state_cache.reset();
}

void CommandStream::CreateStateObjects()
{
	HRESULT hr;

	D3D11_DEPTH_STENCIL_DESC ddesc;
	ddesc.DepthEnable      = FALSE;
	ddesc.DepthWriteMask   = D3D11_DEPTH_WRITE_MASK_ZERO;
	ddesc.DepthFunc        = D3D11_COMPARISON_ALWAYS;
	ddesc.StencilEnable    = FALSE;
	ddesc.StencilReadMask  = D3D11_DEFAULT_STENCIL_READ_MASK;
	ddesc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
	//hr = D3D::device->CreateDepthStencilState(&ddesc, &cleardepthstates[0]);
	//CHECK(hr==S_OK, "Create depth state for Renderer::ClearScreen");
	ddesc.DepthWriteMask   = D3D11_DEPTH_WRITE_MASK_ALL;
	ddesc.DepthEnable      = TRUE;
	hr = D3D::device->CreateDepthStencilState(&ddesc, &m_clear_depth_state_write_enabled);
	CHECK(hr==S_OK, "Create depth state for Renderer::ClearScreen");
	ddesc.DepthWriteMask   = D3D11_DEPTH_WRITE_MASK_ZERO;
	hr = D3D::device->CreateDepthStencilState(&ddesc, &m_clear_depth_state_write_disabled);
	CHECK(hr==S_OK, "Create depth state for Renderer::ClearScreen");
	//D3D::SetDebugObjectName((ID3D11DeviceChild*)cleardepthstates[0], "depth state for Renderer::ClearScreen (depth buffer disabled)");
	D3D::SetDebugObjectName((ID3D11DeviceChild*)m_clear_depth_state_write_enabled.Get(), "depth state for Renderer::ClearScreen (depth buffer enabled, writing enabled)");
	D3D::SetDebugObjectName((ID3D11DeviceChild*)m_clear_depth_state_write_disabled.Get(), "depth state for Renderer::ClearScreen (depth buffer enabled, writing disabled)");

	D3D11_BLEND_DESC blenddesc;
	blenddesc.AlphaToCoverageEnable = FALSE;
	blenddesc.IndependentBlendEnable = FALSE;
	blenddesc.RenderTarget[0].BlendEnable = FALSE;
	blenddesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	blenddesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blenddesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
	blenddesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blenddesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blenddesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blenddesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	hr = D3D::device->CreateBlendState(&blenddesc, &m_default_blend_state);
	CHECK(hr==S_OK, "Create blend state for Renderer::ResetAPIState");
	D3D::SetDebugObjectName((ID3D11DeviceChild*)m_default_blend_state.Get(), "blend state for Renderer::ResetAPIState");

	//clearblendstates[0] = resetblendstate;
	//resetblendstate->AddRef();

	blenddesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED|D3D11_COLOR_WRITE_ENABLE_GREEN|D3D11_COLOR_WRITE_ENABLE_BLUE;
	hr = D3D::device->CreateBlendState(&blenddesc, &m_clear_color_blend_state);
	CHECK(hr==S_OK, "Create blend state for Renderer::ClearScreen");

	blenddesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
	hr = D3D::device->CreateBlendState(&blenddesc, &m_clear_alpha_blend_state);
	CHECK(hr==S_OK, "Create blend state for Renderer::ClearScreen");

	blenddesc.RenderTarget[0].RenderTargetWriteMask = 0;
	hr = D3D::device->CreateBlendState(&blenddesc, &m_clear_depth_blend_state);
	CHECK(hr==S_OK, "Create blend state for Renderer::ClearScreen");

	ddesc.DepthEnable      = FALSE;
	ddesc.DepthWriteMask   = D3D11_DEPTH_WRITE_MASK_ZERO;
	ddesc.DepthFunc        = D3D11_COMPARISON_LESS;
	ddesc.StencilEnable    = FALSE;
	ddesc.StencilReadMask  = D3D11_DEFAULT_STENCIL_READ_MASK;
	ddesc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
	hr = D3D::device->CreateDepthStencilState(&ddesc, &m_default_depth_state);
	CHECK(hr==S_OK, "Create depth state for Renderer::ResetAPIState");
	D3D::SetDebugObjectName((ID3D11DeviceChild*)m_default_depth_state.Get(), "depth stencil state for Renderer::ResetAPIState");

	D3D11_RASTERIZER_DESC rastdesc = CD3D11_RASTERIZER_DESC(D3D11_FILL_SOLID, D3D11_CULL_NONE, false, 0, 0.f, 0.f, false, false, false, false);
	hr = D3D::device->CreateRasterizerState(&rastdesc, &m_default_rasterizer_state);
	CHECK(hr==S_OK, "Create rasterizer state for Renderer::ResetAPIState");
	D3D::SetDebugObjectName((ID3D11DeviceChild*)m_default_rasterizer_state.Get(), "rasterizer state for Renderer::ResetAPIState");

	D3D::stateman->SetRasterizerState(m_default_rasterizer_state.Get());
	D3D::stateman->SetDepthState(m_default_depth_state.Get());
	D3D::stateman->SetBlendState(m_default_blend_state.Get());

	// Default values
	m_save_viewport.TopLeftX = 0.0f;
	m_save_viewport.TopLeftY = 0.0f;
	m_save_viewport.Width = EFB_WIDTH;
	m_save_viewport.Height = EFB_HEIGHT;
	m_save_viewport.MinDepth = 0.0f;
	m_save_viewport.MaxDepth = 1.0f;
	m_save_scissor_rect.left = 0;
	m_save_scissor_rect.right = EFB_WIDTH;
	m_save_scissor_rect.top = 0;
	m_save_scissor_rect.bottom = EFB_HEIGHT;
}

void CommandStream::DeleteStateObjects()
{

}

template<class T>
T* CommandStream::AllocateCommand(size_t required_aux_space /*= 0*/)
{
	size_t required = sizeof(T) + required_aux_space + COMMAND_ALLOCATION_ALIGNMENT;
	size_t start_wpos = m_command_buffer_wpos.load();
	size_t space = COMMAND_BUFFER_SIZE - start_wpos;
	if (space < required)
	{
		// This is really terrible.
		WARN_LOG(VIDEO, "Command stream buffer overflow, flushing.");
		ResetBuffer();
		start_wpos = m_command_buffer_wpos.load();
	}

	T* ptr = new (m_command_buffer + start_wpos) T();
	ptr->Size = sizeof(T);
	return ptr;
}

void* CommandStream::AllocateCommandAux(BaseData* cmd, size_t count)
{
	size_t start_wpos = m_command_buffer_wpos.load() + cmd->Size;
	_assert_((start_wpos + count + COMMAND_ALLOCATION_ALIGNMENT) <= COMMAND_BUFFER_SIZE);
	cmd->Size += count;
	return (m_command_buffer + start_wpos);
}

void CommandStream::EnqueueCommand(BaseData* cmd)
{
	size_t start_wpos = m_command_buffer_wpos.load();
	size_t end_wpos = start_wpos + cmd->Size;
	size_t padding = end_wpos % COMMAND_ALLOCATION_ALIGNMENT;
	if (padding)
		end_wpos += COMMAND_ALLOCATION_ALIGNMENT - padding;

	cmd->Size = end_wpos - start_wpos;
	m_command_buffer_wpos.fetch_add(cmd->Size);

	if (m_use_worker_thread)
	{
		m_worker_control.Wakeup();
	}
	else
	{
		ExecCommand(cmd);
		DeallocateCommand(cmd);
		ResetBuffer();
	}
}

bool CommandStream::DequeueCommand(BaseData** cmd)
{
	size_t rpos = m_command_buffer_rpos.load();
	size_t wpos = m_command_buffer_wpos.load();
	if (rpos == wpos)
		return false;

	*cmd = reinterpret_cast<BaseData*>(m_command_buffer + rpos);
	return true;
}

void CommandStream::DeallocateCommand(BaseData* cmd)
{
	cmd->~BaseData();
	m_command_buffer_rpos.fetch_add(cmd->Size);
}

void CommandStream::ResetBuffer()
{
	if (m_use_worker_thread)
	{
		do
		{
			m_worker_control.Wait();
		}
		while (m_command_buffer_rpos.load() != m_command_buffer_wpos.load());
	}

	_assert_(m_command_buffer_rpos.load() == m_command_buffer_wpos.load());
	m_command_buffer_rpos.store(0);
	m_command_buffer_wpos.store(0);
}

void CommandStream::BackendThreadLoop()
{
	static const u32 MAX_SPIN_COUNT = 100;
	u32 spin_count = 0;

	m_worker_control.Run([this, &spin_count]
	{
		// Drain queue
		for (;;)
		{
			BaseData* cmd;
			if (!DequeueCommand(&cmd))
			{
				spin_count++;
				if (spin_count >= MAX_SPIN_COUNT)
					m_worker_control.AllowSleep();

				break;
			}

			ExecCommand(cmd);
			DeallocateCommand(cmd);
			spin_count = 0;
		}
	}, 100);
}

void CommandStream::ResetAPIState()
{
	D3D::stateman->SetRasterizerState(m_default_rasterizer_state.Get());
	D3D::stateman->SetDepthState(m_default_depth_state.Get());
	D3D::stateman->SetBlendState(m_default_blend_state.Get());
}

void CommandStream::RestoreAPIState()
{
	D3D::stateman->SetRasterizerState(m_save_rasterizer_state.Get());
	D3D::stateman->SetDepthState(m_save_depth_state.Get());
	D3D::stateman->SetBlendState(m_save_blend_state.Get());
	D3D::stateman->SetVertexShader(m_save_vertex_shader.Get());
	D3D::stateman->SetVertexConstants(VertexShaderCache::GetConstantBuffer());
	D3D::stateman->SetGeometryShader(m_save_geometry_shader.Get());
	D3D::stateman->SetGeometryConstants(GeometryShaderCache::GetConstantBuffer());
	D3D::stateman->SetPixelShader(m_save_pixel_shader.Get());
	D3D::stateman->SetPixelConstants(PixelShaderCache::GetConstantBuffer(), VertexShaderCache::GetConstantBuffer());
	D3D::stateman->SetViewport(m_save_viewport);
	D3D::stateman->SetScissorRect(m_save_scissor_rect);
	D3D::stateman->SetRenderTarget(FramebufferManager::GetEFBColorTexture()->GetRTV(), FramebufferManager::GetEFBDepthTexture()->GetDSV());

	for (u32 i = 0; i < 8; i++)
	{
		D3D::stateman->SetTexture(i, m_save_textures[i].Get());
		D3D::stateman->SetSampler(i, m_save_samplers[i].Get());
	}
}

void CommandStream::ExecCommand(BaseData* cmd)
{
	switch (cmd->Type)
	{
	case CmdUploadTexture:			ExecUploadTexture(static_cast<UploadTextureData*>(cmd));					break;
	case CmdCopyEFBToTexture:		ExecCopyEFBToTexture(static_cast<CopyEFBToTextureData*>(cmd));				break;
	case CmdLoadVertexConstants:	ExecLoadVertexConstants(static_cast<LoadVertexConstantsData*>(cmd));		break;
	case CmdLoadGeometryConstants:	ExecLoadGeometryConstants(static_cast<LoadGeometryConstantsData*>(cmd));	break;
	case CmdLoadPixelConstants:		ExecLoadPixelConstants(static_cast<LoadPixelConstantsData*>(cmd));			break;
	case CmdLoadViewport:			ExecLoadViewport(static_cast<LoadViewportData*>(cmd));						break;
	case CmdLoadScissorRect:		ExecLoadScissorRect(static_cast<LoadScissorRectData*>(cmd));				break;
	case CmdClearScreen:			ExecClearScreen(static_cast<ClearScreenData*>(cmd));						break;
	case CmdSetPipeline:			ExecSetPipeline(static_cast<SetPipelineData*>(cmd));						break;
	case CmdBindTexture:			ExecBindTexture(static_cast<BindTextureData*>(cmd));						break;
	case CmdSetSampler:				ExecSetSampler(static_cast<SetSamplerData*>(cmd));							break;
	case CmdDraw:					ExecDraw(static_cast<DrawData*>(cmd));										break;
	case CmdSwap:					ExecSwap(static_cast<SwapData*>(cmd));										break;

	default:
		PanicAlert("Unknown command");
		break;
	}
}

void CommandStream::UploadTexture(TextureCache::TCacheEntry* entry, u32 width, u32 height, u32 row_pitch, u32 level, const u8* data)
{
	u32 data_size = row_pitch * height;

	auto cmd = AllocateCommand<UploadTextureData>(data_size);
	cmd->Entry = entry;
	cmd->Width = width;
	cmd->Height = height;
	cmd->RowPitch = row_pitch;
	cmd->Level = level;
	cmd->Data = (u8*)AllocateCommandAux(cmd, data_size);
	memcpy(cmd->Data, data, data_size);
	EnqueueCommand(cmd);
}

void CommandStream::ExecUploadTexture(const UploadTextureData* data)
{
	D3D::ReplaceRGBATexture2D(data->Entry->texture->GetTex(), data->Data, data->Width, data->Height, data->RowPitch, data->Level, data->Entry->usage);
}

void CommandStream::CopyEFBToTexture(TextureCache::TCacheEntry* entry, PEControl::PixelFormat srcFormat, const EFBRectangle &srcRect, bool scaleByHalf, const float* colmat, u32 colmatidx)
{
	auto cmd = AllocateCommand<CopyEFBToTextureData>();
	cmd->DstTextureSRV = entry->texture->GetSRV();
	cmd->DstTextureSRV->AddRef();
	cmd->DstTextureRTV = entry->texture->GetRTV();
	cmd->DstTextureRTV->AddRef();
	cmd->TextureWidth = entry->config.width;
	cmd->TextureHeight = entry->config.height;
	cmd->SrcFormat = srcFormat;
	cmd->SrcRect = srcRect;
	cmd->ScaleByHalf = scaleByHalf;
	memcpy(cmd->ColMat.data(), colmat, sizeof(float) * cmd->ColMat.size());
	cmd->ColMatIdx = colmatidx;
	EnqueueCommand(cmd);
}

void CommandStream::LoadVertexConstants(const VertexShaderConstants* data)
{
	auto cmd = AllocateCommand<LoadVertexConstantsData>();
	memcpy(&cmd->Data, data, sizeof(VertexShaderConstants));
	EnqueueCommand(cmd);
}

void CommandStream::ExecLoadVertexConstants(const LoadVertexConstantsData* data)
{
	ID3D11Buffer* buffer = VertexShaderCache::GetConstantBuffer();

	D3D11_MAPPED_SUBRESOURCE map;
	D3D::context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	memcpy(map.pData, &data->Data, sizeof(VertexShaderConstants));
	D3D::context->Unmap(buffer, 0);

	ADDSTAT(stats.thisFrame.bytesUniformStreamed, sizeof(VertexShaderConstants));
}

void CommandStream::LoadGeometryConstants(const GeometryShaderConstants* data)
{
	auto cmd = AllocateCommand<LoadGeometryConstantsData>();
	memcpy(&cmd->Data, data, sizeof(GeometryShaderConstants));
	EnqueueCommand(cmd);
}

void CommandStream::ExecLoadGeometryConstants(const LoadGeometryConstantsData* data)
{
	ID3D11Buffer* buffer = GeometryShaderCache::GetConstantBuffer();

	D3D11_MAPPED_SUBRESOURCE map;
	D3D::context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	memcpy(map.pData, &data->Data, sizeof(GeometryShaderConstants));
	D3D::context->Unmap(buffer, 0);

	ADDSTAT(stats.thisFrame.bytesUniformStreamed, sizeof(GeometryShaderConstants));
}

void CommandStream::LoadPixelConstants(const PixelShaderConstants* data)
{
	auto cmd = AllocateCommand<LoadPixelConstantsData>();
	memcpy(&cmd->Data, data, sizeof(PixelShaderConstants));
	EnqueueCommand(cmd);
}

void CommandStream::ExecLoadPixelConstants(const LoadPixelConstantsData* data)
{
	ID3D11Buffer* buffer = PixelShaderCache::GetConstantBuffer();

	D3D11_MAPPED_SUBRESOURCE map;
	D3D::context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	memcpy(map.pData, &data->Data, sizeof(PixelShaderConstants));
	D3D::context->Unmap(buffer, 0);

	ADDSTAT(stats.thisFrame.bytesUniformStreamed, sizeof(PixelShaderConstants));
}

void CommandStream::LoadViewport(const D3D11_VIEWPORT* viewport)
{
	auto cmd = AllocateCommand<LoadViewportData>();
	memcpy(&cmd->Viewport, viewport, sizeof(D3D11_VIEWPORT));
	EnqueueCommand(cmd);
}

void CommandStream::ExecLoadViewport(const LoadViewportData* data)
{
	D3D::stateman->SetViewport(data->Viewport);
	memcpy(&m_save_viewport, &data->Viewport, sizeof(D3D11_VIEWPORT));
}

void CommandStream::LoadScissorRect(const D3D11_RECT* data)
{
	auto cmd = AllocateCommand<LoadScissorRectData>();
	memcpy(&cmd->ScissorRect, data, sizeof(D3D11_RECT));
	EnqueueCommand(cmd);
}

void CommandStream::ExecLoadScissorRect(const LoadScissorRectData* data)
{
	D3D::stateman->SetScissorRect(data->ScissorRect);
	memcpy(&m_save_scissor_rect, &data->ScissorRect, sizeof(D3D11_RECT));
}

void CommandStream::ClearScreen(const EFBRectangle& rc, bool colorEnable, bool alphaEnable, bool zEnable, u32 color, u32 z)
{
	auto cmd = AllocateCommand<ClearScreenData>();
	cmd->Rect = rc;
	cmd->ColorValue = color;
	cmd->ZValue = z;
	cmd->ColorEnable = colorEnable;
	cmd->AlphaEnable = alphaEnable;
	cmd->ZEnable = zEnable;
	EnqueueCommand(cmd);
}

void CommandStream::ExecClearScreen(const ClearScreenData* data)
{
	ResetAPIState();

	if (data->ColorEnable && data->AlphaEnable) D3D::stateman->SetBlendState(m_default_blend_state.Get());
	else if (data->ColorEnable) D3D::stateman->SetBlendState(m_clear_color_blend_state.Get());
	else if (data->AlphaEnable) D3D::stateman->SetBlendState(m_clear_alpha_blend_state.Get());
	else D3D::stateman->SetBlendState(m_clear_depth_blend_state.Get());

	// TODO: Should we enable Z testing here?
	/*if (!bpmem.zmode.testenable) D3D::stateman->PushDepthState(cleardepthstates[0]);
	else */if (data->ZEnable) D3D::stateman->SetDepthState(m_clear_depth_state_write_enabled.Get());
	else /*if (!data->ZEnable)*/ D3D::stateman->SetDepthState(m_clear_depth_state_write_disabled.Get());

	// Update the view port for clearing the picture
	TargetRectangle targetRc = g_renderer->ConvertEFBRectangle(data->Rect);
	D3D11_VIEWPORT vp = CD3D11_VIEWPORT((float)targetRc.left, (float)targetRc.top, (float)targetRc.GetWidth(), (float)targetRc.GetHeight(), 0.f, 1.f);
	D3D::stateman->SetViewport(vp);

	// Color is passed in bgra mode so we need to convert it to rgba
	u32 color = data->ColorValue;
	u32 rgbaColor = (color & 0xFF00FF00) | ((color >> 16) & 0xFF) | ((color << 16) & 0xFF0000);
	D3D::drawClearQuad(rgbaColor, 1.0f - (data->ZValue & 0xFFFFFF) / 16777216.0f);

	RestoreAPIState();
}

void CommandStream::BindTexture(u32 index, ID3D11ShaderResourceView* texture)
{
	if (m_last_textures[index].Get() == texture)
		return;

	m_last_textures[index] = texture;

	if (texture)
		texture->AddRef();

	auto cmd = AllocateCommand<BindTextureData>();
	cmd->Texture = texture;
	cmd->Index = index;
	EnqueueCommand(cmd);
}

void CommandStream::ExecBindTexture(const BindTextureData* data)
{
	// TODO: Refcounting issues here... Perhaps we should AddRef in stateman, or flush.
	D3D::stateman->SetTexture(data->Index, data->Texture);
	if (data->Texture)
		data->Texture->Release();

	m_save_textures[data->Index] = data->Texture;
}

void CommandStream::SetSampler(u32 index, const SamplerState* state)
{
	auto cmd = AllocateCommand<SetSamplerData>();
	memcpy(&cmd->State, state, sizeof(SamplerState));
	cmd->Index = index;
	EnqueueCommand(cmd);
}

void CommandStream::ExecSetSampler(const SetSamplerData* data)
{
	ID3D11SamplerState* ss = g_state_cache->Get(data->State);
	D3D::stateman->SetSampler(data->Index, ss);

	m_save_samplers[data->Index] = ss;
}

void CommandStream::SetPipeline(const RasterizerState* rstate, const ZMode* dstate, const BlendState* bstate, ID3D11InputLayout* input_layout, ID3D11VertexShader* vertex_shader, ID3D11GeometryShader* geometry_shader, ID3D11PixelShader* pixel_shader)
{
	auto cmd = AllocateCommand<SetPipelineData>();
	memcpy(&cmd->RState, rstate, sizeof(cmd->RState));
	memcpy(&cmd->DState, dstate, sizeof(cmd->DState));
	memcpy(&cmd->BState, bstate, sizeof(cmd->BState));
	cmd->InputLayout = input_layout;
	cmd->VertexShader = vertex_shader;
	cmd->GeometryShader = geometry_shader;
	cmd->PixelShader = pixel_shader;
	EnqueueCommand(cmd);
}

void CommandStream::ExecSetPipeline(const SetPipelineData* data)
{
	if (m_current_rasterizer_state.packed != data->RState.packed)
	{
		ID3D11RasterizerState* rs = g_state_cache->Get(data->RState);
		D3D::stateman->SetRasterizerState(rs);

		m_current_rasterizer_state.packed = data->RState.packed;
		m_save_rasterizer_state = rs;
	}

	if (memcmp(&m_current_depth_state, &data->DState, sizeof(m_current_depth_state)))
	{
		ID3D11DepthStencilState* ds = g_state_cache->Get(data->DState);
		D3D::stateman->SetDepthState(ds);

		memcpy(&m_current_depth_state, &data->DState, sizeof(m_current_depth_state));
		m_save_depth_state = ds;
	}

	if (m_current_blend_state.packed != data->BState.packed)
	{
		ID3D11BlendState* bs = g_state_cache->Get(data->BState);
		D3D::stateman->SetBlendState(bs);

		m_current_blend_state.packed = data->BState.packed;
		m_save_blend_state = bs;
	}

	D3D::stateman->SetInputLayout(data->InputLayout);
	D3D::stateman->SetVertexShader(data->VertexShader);
	D3D::stateman->SetVertexConstants(VertexShaderCache::GetConstantBuffer());
	D3D::stateman->SetGeometryConstants(GeometryShaderCache::GetConstantBuffer());
	D3D::stateman->SetGeometryShader(data->GeometryShader);
	D3D::stateman->SetPixelConstants(PixelShaderCache::GetConstantBuffer(), VertexShaderCache::GetConstantBuffer());
	D3D::stateman->SetPixelShader(data->PixelShader);

	m_save_input_layout = data->InputLayout;
	m_save_vertex_shader = data->VertexShader;
	m_save_geometry_shader = data->GeometryShader;
	m_save_pixel_shader = data->PixelShader;
}

void CommandStream::Draw(PrimitiveType primitive, const u8* vertex_data, u32 vertex_data_size, u32 vertex_stride, const u16* index_data, u32 index_count)
{
	u32 index_data_size = sizeof(u16) * index_count;
	u32 aux_size = vertex_data_size + index_data_size;

	auto cmd = AllocateCommand<DrawData>(aux_size);
	cmd->Primitive = primitive;
	cmd->VertexData = (u8*)AllocateCommandAux(cmd, vertex_data_size);
	cmd->VertexDataSize = vertex_data_size;
	cmd->VertexStride = vertex_stride;
	memcpy(cmd->VertexData, vertex_data, vertex_data_size);
	cmd->IndexData = (u16*)AllocateCommandAux(cmd, index_data_size);
	cmd->IndexCount = index_count;
	memcpy(cmd->IndexData, index_data, index_data_size);
	EnqueueCommand(cmd);
}

void CommandStream::ExecDraw(const DrawData* data)
{
	if (data->Primitive != PRIMITIVE_TRIANGLES)
	{
		RasterizerState temp;
		temp.packed = m_current_rasterizer_state.packed;
		temp.cull_mode = D3D11_CULL_NONE;
		D3D::stateman->SetRasterizerState(g_state_cache->Get(temp));
	}

	VertexManager::Draw(data->Primitive, data->VertexData, data->VertexDataSize, data->VertexStride, data->IndexData, data->IndexCount);

	if (data->Primitive != PRIMITIVE_TRIANGLES)
		D3D::stateman->SetRasterizerState(m_save_rasterizer_state.Get());
}

void CommandStream::Swap(u32 xfbAddr, u32 fbWidth, u32 fbStride, u32 fbHeight, const EFBRectangle& rc, float gamma)
{
	auto cmd = AllocateCommand<SwapData>();
	cmd->XFBAddr = xfbAddr;
	cmd->FBWidth = fbWidth;
	cmd->FBStride = fbStride;
	cmd->FBHeight = fbHeight;
	cmd->EFBRect = rc;
	cmd->Gamma = gamma;
	EnqueueCommand(cmd);

	ResetBuffer();
}

void CommandStream::ExecSwap(const SwapData* data)
{
	ResetAPIState();

	static_cast<DX11::Renderer*>(g_renderer.get())->RealSwapImpl(data->XFBAddr, data->FBWidth, data->FBStride, data->FBHeight, data->EFBRect, data->Gamma);

	RestoreAPIState();
}

}
