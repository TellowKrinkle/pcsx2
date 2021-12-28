/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#ifndef __OBJC__
	#error "This header is for use with Objective-C++ only.
#endif

#ifdef __APPLE__

#include "GS/GS.h"
#include "GSMTLSharedHeader.h"
#include "MTLDrawableFetcher.h"
#include <AppKit/AppKit.h>
#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>
#include <atomic>
#include <memory>
#include <unordered_map>

struct PipelineSelectorExtrasMTL
{
	union
	{
		struct
		{
			u8 blend;
			GSTexture::Format rt : 4;
			u8 writemask : 4;
			bool has_depth : 1;
			bool has_stencil : 1;
			bool accumulation_blend : 1;
			bool mixed_hw_sw_blend : 1;
		};
		u32 key;
	};
	PipelineSelectorExtrasMTL(): key(0) {}
	PipelineSelectorExtrasMTL(GSHWDrawConfig::BlendState blend, GSTexture* rt, GSHWDrawConfig::ColorMaskSelector cms, bool has_depth, bool has_stencil)
		: key(0)
	{
		this->blend = blend.index;
		this->rt = rt ? rt->GetFormat() : GSTexture::Format::Invalid;
		MTLColorWriteMask mask = MTLColorWriteMaskNone;
		if (cms.wr) mask |= MTLColorWriteMaskRed;
		if (cms.wg) mask |= MTLColorWriteMaskGreen;
		if (cms.wb) mask |= MTLColorWriteMaskBlue;
		if (cms.wa) mask |= MTLColorWriteMaskAlpha;
		this->writemask = mask;
		this->has_depth = has_depth;
		this->has_stencil = has_stencil;
		this->accumulation_blend = blend.is_accumulation;
		this->mixed_hw_sw_blend = blend.is_mixed_hw_sw;
	}
};
struct PipelineSelectorMTL
{
	GSHWDrawConfig::PSSelector ps;
	PipelineSelectorExtrasMTL extras;
	GSHWDrawConfig::VSSelector vs;
	PipelineSelectorMTL()
	{
		memset(this, 0, sizeof(*this));
	}
	PipelineSelectorMTL(GSHWDrawConfig::VSSelector vs, GSHWDrawConfig::PSSelector ps, PipelineSelectorExtrasMTL extras)
	{
		memset(this, 0, sizeof(*this));
		this->vs = vs;
		this->ps = ps;
		this->extras = extras;
	}
	PipelineSelectorMTL(const PipelineSelectorMTL& other)
	{
		memcpy(this, &other, sizeof(other));
	}
	PipelineSelectorMTL& operator=(const PipelineSelectorMTL& other)
	{
		memcpy(this, &other, sizeof(other));
		return *this;
	}
	bool operator==(const PipelineSelectorMTL& other) const
	{
		return BitEqual(*this, other);
	}
};

template <>
struct std::hash<PipelineSelectorMTL>
{
	size_t operator()(const PipelineSelectorMTL& sel) const
	{
		const char* ptr = reinterpret_cast<const char*>(&sel);
		return std::hash<std::string_view>()(std::string_view(ptr, sizeof(PipelineSelectorMTL)));
	}
};

class GSScopedDebugGroupMTL
{
	id<MTLCommandBuffer> m_buffer;
public:
	GSScopedDebugGroupMTL(id<MTLCommandBuffer> buffer, NSString* name): m_buffer(buffer)
	{
		[m_buffer pushDebugGroup:name];
	}
	~GSScopedDebugGroupMTL()
	{
		[m_buffer popDebugGroup];
	}
};

class GSTextureMTL;

class GSDeviceMTL final : public GSDevice
{
public:
	using DepthStencilSelector = GSHWDrawConfig::DepthStencilSelector;
	using SamplerSelector = GSHWDrawConfig::SamplerSelector;
	enum class LoadAction
	{
		DontCare,
		DontCareIfFull,
		Load,
	};
	class UsageTracker
	{
		struct UsageEntry
		{
			u64 drawno;
			size_t pos;
		};
		std::vector<UsageEntry> m_usage;
		size_t m_size = 0;
		size_t m_pos = 0;
	public:
		size_t Size() { return m_size; }
		size_t Pos() { return m_pos; }
		bool PrepareForAllocation(u64 last_draw, size_t amt);
		size_t Allocate(u64 current_draw, size_t amt);
		void Reset(size_t new_size);
	};
	struct Map
	{
		id<MTLBuffer> gpu_buffer;
		size_t gpu_offset;
		void* cpu_buffer;
	};
	struct UploadBuffer
	{
		UsageTracker usage;
		id<MTLBuffer> mtlbuffer;
		void* buffer = nullptr;
	};
	struct BufferPair
	{
		UsageTracker usage;
		id<MTLBuffer> cpubuffer;
		id<MTLBuffer> gpubuffer;
		void* buffer = nullptr;
		size_t last_upload = 0;
	};

	struct ConvertShaderVertex
	{
		simd_float2 pos;
		simd_float2 texpos;
	};

	struct VSSelector
	{
		union
		{
			struct
			{
				bool iip : 1;
				bool fst : 1;
			};
			u8 key;
		};
		VSSelector(): key(0) {}
		VSSelector(u8 key): key(key) {}
	};

	struct OutlivesDeviceObj
	{
		std::mutex mtx;
		GSDeviceMTL* backref;
		dispatch_semaphore_t gpu_work_sema;
		OutlivesDeviceObj(GSDeviceMTL* dev);
	};

	// MARK: Configuration
	bool m_unified_memory;
	TriFiltering m_filter;
	int m_mipmap;
	int m_max_texsize;
	u32 m_capture_frame;

	// MARK: Permanent resources
	std::shared_ptr<OutlivesDeviceObj> m_outlive;
	MTLDrawableFetcher m_drawable_fetcher;
	id<MTLDevice> m_dev;
	id<MTLCommandQueue> m_queue;
	id<MTLFence> m_draw_sync_fence;
	NSView* m_view;
	CAMetalLayer* m_layer;
	id<MTLLibrary> m_shaders;
	MTLFunctionConstantValues* m_fn_constants;
	MTLVertexDescriptor* m_hw_vertex;
	std::unique_ptr<GSTextureMTL> m_font;

	// Draw IDs are used to make sure we're not clobbering things
	u64 m_current_draw = 1;
	std::atomic<u64> m_last_finished_draw{0};

	// Functions and Pipeline States
	id<MTLRenderPipelineState> m_convert_pipeline[static_cast<int>(ShaderConvert::Count)];
	id<MTLRenderPipelineState> m_convert_pipeline_copy[static_cast<int>(GSTexture::Format::Last) + 1];
	id<MTLRenderPipelineState> m_convert_pipeline_copy_mask[1 << 4];
	id<MTLRenderPipelineState> m_convert_pipeline_f2i[2];
	id<MTLRenderPipelineState> m_merge_pipeline[2];
	id<MTLRenderPipelineState> m_interlace_pipeline[4];

	id<MTLFunction> m_hw_vs[1 << 2];
	std::unordered_map<u64, id<MTLFunction>> m_hw_ps;
	std::unordered_map<PipelineSelectorMTL, id<MTLRenderPipelineState>> m_hw_pipeline;

	MTLRenderPassDescriptor* m_render_pass_desc[8];

	id<MTLSamplerState> m_sampler_hw[1 << 7];

	id<MTLDepthStencilState> m_dss_destination_alpha;
	id<MTLDepthStencilState> m_dss_hw[1 << 5];

	id<MTLBuffer> m_texture_download_buf;
	UploadBuffer m_texture_upload_buf;
	BufferPair m_vertex_upload_buf;

	// MARK: Ephemeral resources
	id<MTLCommandBuffer> m_current_render_cmdbuf;
	struct MainRenderEncoder
	{
		id<MTLRenderCommandEncoder> encoder;
		GSTexture* color_target = nullptr;
		GSTexture* depth_target = nullptr;
		GSTexture* stencil_target = nullptr;
		GSTexture* tex[8] = {};
		GSVector4i scissor;
		void* vertex_buffer = nullptr;
		void* pipeline = nullptr;
		void* depth = nullptr;
		PipelineSelectorMTL pipeline_sel;
		DepthStencilSelector depth_sel = DepthStencilSelector::NoDepth();
		SamplerSelector sampler_sel;
		GSHWDrawConfig::VSConstantBuffer cb_vs;
		GSHWDrawConfig::PSConstantBuffer cb_ps;
		bool cb_ps_aref_off;
		u8 blend_color;
		bool has_cb_vs = false;
		bool has_cb_ps = false;
		bool has_scissor = false;
		bool has_blend_color = false;
		bool has_pipeline_sel = false;
		bool has_depth_sel = true;
		bool has_sampler = false;
		void SetVertices(id<MTLBuffer> buffer, size_t offset);
		void SetVertexBytes(void* buffer, size_t size);
		void SetScissor(const GSVector4i& scissor);
		void ClearScissor();
		void SetCB(const GSHWDrawConfig::VSConstantBuffer& cb_vs);
		void SetCB(const GSHWDrawConfig::PSConstantBuffer& cb_ps, int atst);
		void SetPSCB(const void* bytes, size_t len);
		void SetBlendColor(u8 blend_color);
		void SetPipeline(id<MTLRenderPipelineState> pipe);
		void SetDepth(id<MTLDepthStencilState> dss);

		MainRenderEncoder(const MainRenderEncoder&) = delete;
		MainRenderEncoder() = default;
	} m_current_render;
	id<MTLCommandBuffer> m_texture_upload_cmdbuf;
	id<MTLBlitCommandEncoder> m_texture_upload_encoder;
	id<MTLBlitCommandEncoder> m_late_texture_upload_encoder;
	id<MTLCommandBuffer> m_vertex_upload_cmdbuf;
	id<MTLBlitCommandEncoder> m_vertex_upload_encoder;

	GSDeviceMTL();
	~GSDeviceMTL() override;

	/// Allocate space in the given buffer
	Map Allocate(UploadBuffer& buffer, size_t amt);
	/// Allocate space in the given buffer for use with the given render command encoder
	Map Allocate(BufferPair& buffer, size_t amt);
	/// Enqueue upload of any outstanding data
	void Sync(BufferPair& buffer);
	/// Get the texture upload encoder, creating a new one if it doesn't exist
	id<MTLBlitCommandEncoder> GetTextureUploadEncoder();
	/// Get the late texture upload encoder, creating a new one if it doesn't exist
	id<MTLBlitCommandEncoder> GetLateTextureUploadEncoder();
	/// Get the vertex upload encoder, creating a new one if it doesn't exist
	id<MTLBlitCommandEncoder> GetVertexUploadEncoder();
	/// Get the render command buffer, creating a new one if it doesn't exist
	id<MTLCommandBuffer> GetRenderCmdBuf();
	/// Flush pending operations from all encoders to the GPU
	void FlushEncoders();
	/// End current render pass without flushing
	void EndRenderPass();
	/// Begin a new render pass (may reuse existing)
	MainRenderEncoder& BeginRenderPass(GSTexture* color, MTLLoadAction color_load, GSTexture* depth, MTLLoadAction depth_load, GSTexture* stencil = nullptr, MTLLoadAction stencil_load = MTLLoadActionDontCare);

	GSTexture* CreateSurface(GSTexture::Type type, int w, int h, GSTexture::Format format) override;
	GSTexture* FetchSurface(GSTexture::Type type, int w, int h, GSTexture::Format format) override;

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, const GSVector4& c) override;
	void DoInterlace(GSTexture* sTex, GSTexture* dTex, int shader, bool linear, float yoffset) override;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) override;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex) override;
	void DoExternalFX(GSTexture* sTex, GSTexture* dTex) override;
	u16 ConvertBlendEnum(u16 generic) override;

	MTLPixelFormat GetPixelFormat(GSTexture::Format);
	id<MTLFunction> LoadShader(NSString* name);
	id<MTLRenderPipelineState> MakePipeline(MTLRenderPipelineDescriptor* desc, id<MTLFunction> vertex, id<MTLFunction> fragment, NSString* name);
	void InitWindow(const WindowInfo& wi);
	bool Create(const WindowInfo& wi) override;
	void GetRealSize(int& w, int& h);
	bool Reset(int w, int h) override;
	void Present(const GSVector4i& r, int shader) override;
	void Present(GSTexture* sTex, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY) override;
	void Flip() override;

	void SetVSync(int vsync) override;

	void ClearRenderTarget(GSTexture* t, const GSVector4& c) override;
	void ClearRenderTarget(GSTexture* t, u32 c) override;
	void ClearDepth(GSTexture* t) override;
	void ClearStencil(GSTexture* t, u8 c) override;

	bool DownloadTexture(GSTexture* src, const GSVector4i& rect, GSTexture::GSMap& out_map) override;

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r) override;
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, id<MTLRenderPipelineState> pipeline, bool linear, LoadAction load_action, void* frag_uniform, size_t frag_uniform_len);
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, ShaderConvert shader = ShaderConvert::COPY, bool linear = true) override;
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha) override;

	void FlushClears(GSTexture* tex);
	void SetHWPipelineState(MainRenderEncoder& enc, GSHWDrawConfig::VSSelector vs, GSHWDrawConfig::PSSelector ps, PipelineSelectorExtrasMTL extras);
	void SetDSS(MainRenderEncoder& enc, DepthStencilSelector sel);
	void SetSampler(MainRenderEncoder& enc, SamplerSelector sel);
	void SetTexture(MainRenderEncoder& enc, GSTexture* tex, int pos);
	void SetupDestinationAlpha(GSTexture* rt, GSTexture* ds, const GSVector4i& r, bool datm);
	void RenderHW(GSHWDrawConfig& config) override;
	void SendHWDraw(GSHWDrawConfig& config, id<MTLRenderCommandEncoder> enc, id<MTLBuffer> buffer, size_t off);

	void RenderOsd(GSTexture* dt) override;
};

#endif // __APPLE__
