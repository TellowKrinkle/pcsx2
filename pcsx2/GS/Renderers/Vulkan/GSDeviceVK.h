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

#include "GSTextureVK.h"
#include "GS/GSVector.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "common/Vulkan/StreamBuffer.h"
#include "common/Vulkan/StagingBuffer.h"
#include "common/HashCombine.h"
#include <array>

class GSDeviceVK final : public GSDevice
{
public:
#pragma pack(push, 1)

	struct alignas(32) VSConstantBuffer
	{
		GSVector4 VertexScale;
		GSVector4 VertexOffset;
		GSVector4 Texture_Scale_Offset;
		GSVector2 PointSize;
		u32 MaxDepth;
		u32 pad;

		VSConstantBuffer()
		{
			VertexScale = GSVector4::zero();
			VertexOffset = GSVector4::zero();
			Texture_Scale_Offset = GSVector4::zero();
			PointSize = GSVector2(0);
			MaxDepth = 0;
			pad = 0;
		}

		__forceinline bool Update(const VSConstantBuffer* cb)
		{
			GSVector4i* a = (GSVector4i*)this;
			GSVector4i* b = (GSVector4i*)cb;

			if (!((a[0] == b[0]) & (a[1] == b[1]) & (a[2] == b[2]) & (a[3] == b[3])).alltrue())
			{
				a[0] = b[0];
				a[1] = b[1];
				a[2] = b[2];
				a[3] = b[3];

				return true;
			}

			return false;
		}
	};

	struct VSSelector
	{
		union
		{
			struct
			{
				u32 tme : 1;
				u32 fst : 1;
				u32 point : 1;

				u32 _free : 29;
			};

			u32 key;
		};

		operator u32() const { return key; }

		VSSelector()
			: key(0)
		{
		}
		VSSelector(u32 k)
			: key(k)
		{
		}
	};

	struct alignas(32) PSConstantBuffer
	{
		GSVector4 FogColor_AREF;
		GSVector4 HalfTexel;
		GSVector4 WH;
		GSVector4 MinMax;
		GSVector4 MinF_TA;
		GSVector4i MskFix;
		GSVector4i ChannelShuffle;
		GSVector4i FbMask;

		GSVector4 TC_OffsetHack;
		GSVector4 Af_MaxDepth;
		GSVector4 DitherMatrix[4];

		PSConstantBuffer()
		{
			FogColor_AREF = GSVector4::zero();
			HalfTexel = GSVector4::zero();
			WH = GSVector4::zero();
			MinMax = GSVector4::zero();
			MinF_TA = GSVector4::zero();
			MskFix = GSVector4i::zero();
			ChannelShuffle = GSVector4i::zero();
			FbMask = GSVector4i::zero();
			Af_MaxDepth = GSVector4::zero();

			DitherMatrix[0] = GSVector4::zero();
			DitherMatrix[1] = GSVector4::zero();
			DitherMatrix[2] = GSVector4::zero();
			DitherMatrix[3] = GSVector4::zero();
		}

		__forceinline bool Update(const PSConstantBuffer* cb)
		{
			GSVector4i* a = (GSVector4i*)this;
			GSVector4i* b = (GSVector4i*)cb;

			if (!((a[0] == b[0]) /*& (a[1] == b1)*/ & (a[2] == b[2]) & (a[3] == b[3]) & (a[4] == b[4]) & (a[5] == b[5]) &
					(a[6] == b[6]) & (a[7] == b[7]) & (a[9] == b[9]) & // if WH matches HalfTexel does too
					(a[10] == b[10]) & (a[11] == b[11]) & (a[12] == b[12]) & (a[13] == b[13]))
					 .alltrue())
			{
				a[0] = b[0];
				a[1] = b[1];
				a[2] = b[2];
				a[3] = b[3];
				a[4] = b[4];
				a[5] = b[5];
				a[6] = b[6];
				a[7] = b[7];
				a[9] = b[9];

				a[10] = b[10];
				a[11] = b[11];
				a[12] = b[12];
				a[13] = b[13];

				return true;
			}

			return false;
		}
	};

	struct GSSelector
	{
		union
		{
			struct
			{
				u32 iip : 1;
				u32 prim : 2;
				u32 point : 1;
				u32 line : 1;
				u32 cpu_sprite : 1;

				u32 _free : 26;
			};

			u32 key;
		};

		operator u32() { return key; }

		GSSelector()
			: key(0)
		{
		}
		GSSelector(u32 k)
			: key(k)
		{
		}

		__fi bool IsNeeded() const
		{
			// Geometry shader is disabled if sprite conversion is done on the cpu (sel.cpu_sprite).
			const bool unscale_pt_ln = (point == 1 || line == 1);
			return ((prim > 0 && cpu_sprite == 0 && (iip == 0 || prim == 3)) || unscale_pt_ln);
		}
	};

	struct PSSelector
	{
		union
		{
			struct
			{
				// *** Word 1
				// Format
				u32 fmt : 4;
				u32 dfmt : 2;
				u32 depth_fmt : 2;
				// Alpha extension/Correction
				u32 aem : 1;
				u32 fba : 1;
				// Fog
				u32 fog : 1;
				// Pixel test
				u32 atst : 3;
				// Color sampling
				u32 fst : 1;
				u32 tfx : 3;
				u32 tcc : 1;
				u32 wms : 2;
				u32 wmt : 2;
				u32 ltf : 1;
				// Shuffle and fbmask effect
				u32 shuffle : 1;
				u32 read_ba : 1;
				u32 fbmask : 1;
				u32 tex_is_fb : 1;

				// Blend and Colclip
				u32 hdr : 1;
				u32 blend_a : 2;
				u32 blend_b : 2; // bit30/31
				u32 blend_c : 2; // bit0
				u32 blend_d : 2;
				u32 clr1 : 1;
				u32 colclip : 1;
				u32 pabe : 1;

				// Others ways to fetch the texture
				u32 channel : 3;

				// Dithering
				u32 dither : 2;

				// Depth clamp
				u32 zclamp : 1;

				// Hack
				u32 tcoffsethack : 1;
				u32 urban_chaos_hle : 1;
				u32 tales_of_abyss_hle : 1;
				u32 point_sampler : 1;
				u32 invalid_tex0 : 1; // Lupin the 3rd
				u32 feedback_loop : 1;

				u32 _free : 12;
			};

			u64 key;
		};

		operator u64() { return key; }

		PSSelector()
			: key(0)
		{
		}
	};

	struct OMDepthStencilSelector
	{
		union
		{
			struct
			{
				u32 ztst : 2;
				u32 zwe : 1;
				u32 date : 1;
				u32 fba : 1;
				u32 date_one : 1;
			};

			u32 key;
		};

		operator u32() { return key & 0x3f; }

		OMDepthStencilSelector()
			: key(0)
		{
		}
	};

	struct OMBlendSelector
	{
		union
		{
			struct
			{
				// Color mask
				u32 wr : 1;
				u32 wg : 1;
				u32 wb : 1;
				u32 wa : 1;
				// Alpha blending
				u32 blend_index : 7;
				u32 abe : 1;
				u32 accu_blend : 1;
			};

			struct
			{
				// Color mask
				u32 wrgba : 4;
			};

			u32 key;
		};

		operator u32() { return key & 0x1fff; }

		OMBlendSelector()
			: key(0)
		{
		}
	};

#pragma pack(pop)

	struct PipelineSelector
	{
		VSSelector vs;
		GSSelector gs;
		PSSelector ps;
		OMDepthStencilSelector dss;
		OMBlendSelector bs;

		union
		{
			struct
			{
				u32 topology : 4;
				u32 rt : 1;
				u32 ds : 1;
			};

			u32 key;
		};

		__fi bool operator==(const PipelineSelector& p) const
		{
			return vs.key == p.vs.key && gs.key == p.gs.key && ps.key == p.ps.key && dss.key == p.dss.key && bs.key == p.bs.key && key == p.key;
		}
		__fi bool operator!=(const PipelineSelector& p) const
		{
			return vs.key != p.vs.key || gs.key != p.gs.key || ps.key != p.ps.key || dss.key != p.dss.key || bs.key != p.bs.key || key != p.key;
		}

		PipelineSelector() : key(0) {}
	};

	struct PipelineSelectorHash
	{
		std::size_t operator()(const PipelineSelector& e) const noexcept
		{
			std::size_t hash = 0;
			hash_combine(hash, e.vs.key, e.gs.key, e.ps.key, e.dss.key, e.bs.key);
			return hash;
		}
	};

	union SamplerSelector
	{
		struct
		{
			u32 filter : 1;
			u32 wrap_u : 3;
			u32 wrap_v : 3;
			u32 anisotropy : 5;
		};

		u32 key;

		SamplerSelector()
			: key(0)
		{
		}
		SamplerSelector(VkFilter filter_, VkSamplerAddressMode wrap_u_, VkSamplerAddressMode wrap_v_, u32 anisotropy_)
		{
			key = 0;
			filter = static_cast<u32>(filter_);
			wrap_u = static_cast<u32>(wrap_u_);
			wrap_v = static_cast<u32>(wrap_v_);
			anisotropy = static_cast<u32>(anisotropy_);
		}
		SamplerSelector(const SamplerSelector& s)
			: key(s.key)
		{
		}
	};

	enum : u32
	{
		NUM_TFX_DESCRIPTOR_SETS = 3,
		NUM_TFX_DYNAMIC_OFFSETS = 2,
		NUM_TFX_SAMPLERS = 2,
		NUM_TFX_RT_TEXTURES = 2,
		NUM_TFX_TEXTURES = NUM_TFX_SAMPLERS + NUM_TFX_RT_TEXTURES,
		NUM_CONVERT_TEXTURES = 1,
		NUM_CONVERT_SAMPLERS = 1,
		CONVERT_PUSH_CONSTANTS_SIZE = 32,

		TEXTURE_BUFFER_SIZE = 64 * 1024 * 1024,
		VERTEX_BUFFER_SIZE = 32 * 1024 * 1024,
		INDEX_BUFFER_SIZE = 16 * 1024 * 1024,
		VERTEX_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
		FRAGMENT_UNIFORM_BUFFER_SIZE = 8 * 1024 * 1024,
	};

	static constexpr VkFormat RT_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
	static constexpr VkFormat HDR_RT_FORMAT = VK_FORMAT_R32G32B32A32_SFLOAT;
	static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT_S8_UINT;

private:
	float m_hack_topleft_offset;
	int m_upscale_multiplier;
	int m_aniso_filter;
	int m_mipmap;

	VkDescriptorSetLayout m_utility_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_utility_pipeline_layout = VK_NULL_HANDLE;

	VkDescriptorSetLayout m_tfx_ubo_ds_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tfx_sampler_ds_layout = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_tfx_rt_texture_ds_layout = VK_NULL_HANDLE;
	VkPipelineLayout m_tfx_pipeline_layout = VK_NULL_HANDLE;

	Vulkan::StreamBuffer m_texture_upload_buffer;
	Vulkan::StreamBuffer m_vertex_stream_buffer;
	Vulkan::StreamBuffer m_index_stream_buffer;
	Vulkan::StreamBuffer m_vertex_uniform_stream_buffer;
	Vulkan::StreamBuffer m_fragment_uniform_stream_buffer;
	Vulkan::StagingBuffer m_readback_staging_buffer;

	VkSampler m_point_sampler = VK_NULL_HANDLE;
	VkSampler m_linear_sampler = VK_NULL_HANDLE;

	std::unordered_map<u32, VkSampler> m_samplers;

	std::array<VkPipeline, ShaderConvert_Count> m_convert{};
	std::array<VkPipeline, ShaderConvert_Count> m_present{};
	std::array<VkPipeline, 16> m_color_copy{};
	std::array<VkPipeline, 2> m_merge{};
	std::array<VkPipeline, 4> m_interlace{};
	VkPipeline m_hdr_setup_pipelines[2][2] = {};		// [depth][feedback_lopo]
	VkPipeline m_hdr_finish_pipelines[2][2] = {};		// [depth][feedback_loop]

	std::unordered_map<u32, VkShaderModule> m_tfx_vertex_shaders;
	std::unordered_map<u32, VkShaderModule> m_tfx_geometry_shaders;
	std::unordered_map<u64, VkShaderModule> m_tfx_fragment_shaders;
	std::unordered_map<PipelineSelector, VkPipeline, PipelineSelectorHash> m_tfx_pipelines;

	VkRenderPass m_utility_color_render_pass_load = VK_NULL_HANDLE;
	VkRenderPass m_utility_color_render_pass_clear = VK_NULL_HANDLE;
	VkRenderPass m_utility_color_render_pass_discard = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_load = VK_NULL_HANDLE;
	VkRenderPass m_utility_depth_render_pass_discard = VK_NULL_HANDLE;
	VkRenderPass m_date_setup_render_pass = VK_NULL_HANDLE;
	VkRenderPass m_swap_chain_render_pass = VK_NULL_HANDLE;

	VkRenderPass m_tfx_render_pass[2][2][2][2][2][3] = {};

	VSConstantBuffer m_vs_cb_cache;
	PSConstantBuffer m_ps_cb_cache;

	std::string m_tfx_source;

	GSTexture* CreateSurface(int type, int w, int h, int format) override;
	GSTexture* FetchSurface(int type, int w, int h, int format) override;

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, const GSVector4& c) final;
	void DoInterlace(GSTexture* sTex, GSTexture* dTex, int shader, bool linear, float yoffset = 0) final;

	u16 ConvertBlendEnum(u16 generic) final;

	VkSampler GetSampler(SamplerSelector ss);

	VkShaderModule GetTFXVertexShader(VSSelector sel);
	VkShaderModule GetTFXGeometryShader(GSSelector sel);
	VkShaderModule GetTFXFragmentShader(PSSelector sel);
	VkPipeline CreateTFXPipeline(const PipelineSelector& p);
	VkPipeline GetTFXPipeline(const PipelineSelector& p);

	VkShaderModule GetUtilityVertexShader(const std::string& source, const char* replace_main);
	VkShaderModule GetUtilityFragmentShader(const std::string& source, const char* replace_main);

	bool CheckFeatures();
	bool CreateNullTexture();
	bool CreateBuffers();
	bool CreatePipelineLayouts();
	bool CreateRenderPasses();

	bool CompileConvertPipelines();
	bool CompileInterlacePipelines();
	bool CompileMergePipelines();

	void DestroyResources();

public:
	GSDeviceVK();
	~GSDeviceVK() override;

	__fi VkRenderPass GetTFXRenderPass(bool rt, bool ds, bool hdr, bool date, bool fbl, VkAttachmentLoadOp op) const { return m_tfx_render_pass[rt][ds][hdr][date][fbl][op]; }
	__fi Vulkan::StreamBuffer& GetTextureUploadBuffer() { return m_texture_upload_buffer; }
	__fi VkSampler GetPointSampler() const { return m_point_sampler; }
	__fi VkSampler GetLinearSampler() const { return m_linear_sampler; }

	bool Create(HostDisplay* display) override;
	void Destroy() override;

	void ResetAPIState() override;
	void RestoreAPIState() override;

	void DrawPrimitive() override;
	void DrawIndexedPrimitive() override;
	void DrawIndexedPrimitive(int offset, int count) override;

	void ClearRenderTarget(GSTexture* t, const GSVector4& c) override;
	void ClearRenderTarget(GSTexture* t, u32 c) override;
	void ClearDepth(GSTexture* t) override;
	void ClearStencil(GSTexture* t, u8 c) override;

	GSTexture* CopyOffscreen(GSTexture* src, const GSVector4& sRect, int w, int h, int format = 0, int ps_shader = 0) override;
	GSTexture* DrawForReadback(GSTexture* src, const GSVector4& sRect, int w, int h, int format = 0, int ps_shader = 0);
	bool ReadbackTexture(GSTexture* src, const GSVector4i& rect, u32 level, GSTexture::GSMap* dst);

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r) override;

	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, int shader = 0, bool linear = true) override;
	void StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, bool red, bool green, bool blue, bool alpha) override;

	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, VkPipeline pipeline, bool linear);
	void DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);

	void BlitRect(GSTexture* sTex, const GSVector4i& sRect, u32 sLevel, GSTexture* dTex, const GSVector4i& dRect, u32 dLevel, bool linear);

	void SetupDATE(GSTexture* rt, GSTexture* ds, const GSVertexPT1* vertices, bool datm, const GSVector4i& bbox);
	void SetupHDR(GSTexture* hdr_rt, GSTexture* rt, GSTexture* ds, const GSVector4i& rect, const GSVector4i& scissor, bool date, bool feedback_loop);
	void FinishHDR(GSTexture* hdr_rt, GSTexture* rt, GSTexture* ds, const GSVector4i& rect, const GSVector4i& scissor, const GSVector4i& render_area, bool date, bool feedback_loop);

	void IASetVertexBuffer(const void* vertex, size_t stride, size_t count);
	bool IAMapVertexBuffer(void** vertex, size_t stride, size_t count);
	void IAUnmapVertexBuffer();
	void IASetIndexBuffer(const void* index, size_t count);

	void PSSetShaderResources(GSTexture* sr0, GSTexture* sr1) override;
	void PSSetShaderResource(int i, GSTexture* sr) override;
	void PSSetSampler(u32 index, SamplerSelector sel);

	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i* scissor = NULL) override;
	void OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i& scissor, bool feedback_loop);

	void SetupVS(const VSConstantBuffer* cb);
	void SetupPS(const PSConstantBuffer* cb);
	bool BindDrawPipeline(const PipelineSelector& p);

	//////////////////////////////////////////////////////////////////////////
	// Vulkan State
	//////////////////////////////////////////////////////////////////////////

public:
	__fi bool CurrentFramebufferHasFeedbackLoop() const { return m_current_framebuffer_has_feedback_loop; }

	/// Ends any render pass, executes the command buffer, and invalidates cached state.
	void ExecuteCommandBuffer(bool wait_for_completion);
	void ExecuteCommandBuffer(bool wait_for_completion, const char* reason, ...);
	void ExecuteCommandBufferAndRestartRenderPass(const char* reason);

	/// Set dirty flags on everything to force re-bind at next draw time.
	void InvalidateCachedState();

	/// Binds all dirty state to the command buffer.
	bool ApplyUtilityState();
	bool ApplyTFXState();

	void SetVertexBuffer(VkBuffer buffer, VkDeviceSize offset);
	void SetIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
	void SetFramebuffer(VkFramebuffer framebuffer);
	void SetBlendConstants(GSVector4 color);

	void SetUtilityTexture(GSTexture* tex, VkSampler sampler);
	void SetUtilityPushConstants(const void* data, u32 size);
	void UnbindTexture(VkImageView view);

	// Ends a render pass if we're currently in one.
	// When Bind() is next called, the pass will be restarted.
	// Calling this function is allowed even if a pass has not begun.
	bool InRenderPass();
	void BeginRenderPass(VkRenderPass rp, const GSVector4i& rect);
	void BeginClearRenderPass(VkRenderPass rp, const GSVector4i& rect, const GSVector4& clear_color);
	bool CheckRenderPassArea(const GSVector4i& rect);
	void EndRenderPass();

	void SetViewport(const VkViewport& viewport);
	void SetViewportFromRect(const GSVector4i& rc);
	void SetViewportAndScissor(const GSVector4i& rc);
	void SetScissor(const GSVector4i& scissor);
	void SetPipeline(VkPipeline pipeline);

private:
	enum DIRTY_FLAG : u32
	{
		DIRTY_FLAG_TFX_SAMPLERS_DS = (1 << 0),
		DIRTY_FLAG_TFX_RT_TEXTURE_DS = (1 << 1),
		DIRTY_FLAG_TFX_DYNAMIC_OFFSETS = (1 << 2),
		DIRTY_FLAG_UTILITY_TEXTURE = (1 << 3),
		DIRTY_FLAG_BLEND_CONSTANTS = (1 << 4),
		DIRTY_FLAG_VERTEX_BUFFER = (1 << 5),
		DIRTY_FLAG_INDEX_BUFFER = (1 << 6),
		DIRTY_FLAG_VIEWPORT = (1 << 7),
		DIRTY_FLAG_SCISSOR = (1 << 8),
		DIRTY_FLAG_PIPELINE = (1 << 9),

		DIRTY_BASE_STATE = DIRTY_FLAG_VERTEX_BUFFER | DIRTY_FLAG_INDEX_BUFFER | DIRTY_FLAG_PIPELINE | DIRTY_FLAG_VIEWPORT | DIRTY_FLAG_SCISSOR | DIRTY_FLAG_BLEND_CONSTANTS,
		DIRTY_TFX_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_TFX_SAMPLERS_DS | DIRTY_FLAG_TFX_RT_TEXTURE_DS,
		DIRTY_UTILITY_STATE = DIRTY_BASE_STATE | DIRTY_FLAG_UTILITY_TEXTURE,
	};

	enum class PipelineLayout
	{
		Undefined,
		TFX,
		Utility
	};

	void InitializeState();
	bool CreatePersistentDescriptorSets();
	bool CreateReadbackTexture();

	void ApplyBaseState(u32 flags, VkCommandBuffer cmdbuf);

	// Which bindings/state has to be updated before the next draw.
	u32 m_dirty_flags = 0;
	bool m_current_framebuffer_has_feedback_loop = false;

	// input assembly
	VkBuffer m_vertex_buffer = VK_NULL_HANDLE;
	VkDeviceSize m_vertex_buffer_offset = 0;
	VkBuffer m_index_buffer = VK_NULL_HANDLE;
	VkDeviceSize m_index_buffer_offset = 0;
	VkIndexType m_index_type = VK_INDEX_TYPE_UINT16;

	VkFramebuffer m_current_framebuffer = VK_NULL_HANDLE;
	VkRenderPass m_current_render_pass = VK_NULL_HANDLE;
	GSVector4i m_current_render_pass_area = GSVector4i::zero();

	VkViewport m_viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
	GSVector4i m_scissor = GSVector4i::zero();
	GSVector4 m_blend_constants = GSVector4::zero();

	std::array<VkImageView, NUM_TFX_TEXTURES> m_tfx_textures{};
	std::array<VkSampler, NUM_TFX_SAMPLERS> m_tfx_samplers{};
	std::array<u32, NUM_TFX_SAMPLERS> m_tfx_sampler_sel{};
	std::array<VkDescriptorSet, NUM_TFX_DESCRIPTOR_SETS> m_tfx_descriptor_sets{};
	std::array<u32, NUM_TFX_DYNAMIC_OFFSETS> m_tfx_dynamic_offsets{};

	VkImageView m_utility_texture = VK_NULL_HANDLE;
	VkSampler m_utility_sampler = VK_NULL_HANDLE;
	VkDescriptorSet m_utility_descriptor_set = VK_NULL_HANDLE;

	PipelineLayout m_current_pipeline_layout = PipelineLayout::Undefined;
	VkPipeline m_current_pipeline = VK_NULL_HANDLE;

	Vulkan::Texture m_null_texture;
};

extern GSDeviceVK* g_vulkan_dev;
