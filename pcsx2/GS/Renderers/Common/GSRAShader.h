// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GSDevice.h"
#include "GSTexture.h"

#include "common/EnumOps.h"

#include <rashader-lite.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

struct GSRAPipeline
{
	GSRAPipeline() = default;
	GSRAPipeline(GSRAPipeline&&) = delete;
protected:
	~GSRAPipeline() = default;
};

struct GSRAFilterChain
{
	enum class UniformFlags
	{
		None = 0,
		AlwaysDirty   = 1 << 0,
		HasInputSize  = 1 << 1,
		HasOutputSize = 1 << 2,
		HasRTSize     = 1 << 3,
		HasUserParam  = 1 << 4,
		HasFPS        = 1 << 5,
	};

	struct UniformMap
	{
		u32 ubo;  ///< Offset of UBO in `uniform_data`
		u32 push; ///< Offset of push in `uniform_data`
		UniformFlags flags_ubo  : 16;
		UniformFlags flags_push : 16;
		u32 extra; ///< For use by backends
	};

	struct UniformInfo
	{
		u32 frame_time_delta;
	};

	GSRAFilterChain() = default;
	GSRAFilterChain(GSRAFilterChain&&) = delete;
protected:
	~GSRAFilterChain();
	void Destroy(void (*pipeline_destruct)(GSRAPipeline*), void (*texture_destruct)(GSTexture*));
public:
	std::string path;
	rashader::FilterChain info;
	rashader::ExecutionPlanner plan;
	std::unordered_map<std::string_view, u32> user_param_names;
private:
	GSRAPipeline** passes_ = nullptr;
	GSTexture** textures_ = nullptr;
	GSTexture** rts_ = nullptr;
	GSTexture** input_history_ = nullptr;
	float* user_params_ = nullptr;
	UniformMap* uniform_offsets_ = nullptr;

public:
	u8* uniform_data = nullptr;
	GSTexture* dummy_tex = nullptr;
	GSVector2 size_input = {};
	GSVector2 size_output = {};
	u32 current_input = 0;

	std::span<GSRAPipeline*> passes() const { return std::span(passes_, info.passes.size()); }
	std::span<GSTexture*> textures() const { return std::span(textures_, info.textures.size()); }
	std::span<GSTexture*> rts() const { return std::span(rts_, plan.num_rts()); }
	std::span<GSTexture*> input_history() const { return std::span(input_history_, plan.input_history_needed() + 1); }
	std::span<float> user_params() const { return std::span(user_params_, info.parameters.size()); }
	std::span<UniformMap> uniform_offsets() const { return std::span(uniform_offsets_, info.passes.size()); }

	/// Initializes structures based on fully-initialized `info` and `plan`
	void Init();
	/// Call after initializing backends and before use
	void FinalizeUniformBuffers();
	/// Removes constant and unused buffer parameters from passes for faster updates
	void CleanConstantBufferParams();
	/// Get the a texture for the given parameters.  Replaces null values with `dummy_tex`.
	GSTexture* GetTexture(rashader::TextureSource source, u32 idx, u32 history) const;
	/// Get the size that the given RT should be resized to for this pass
	GSVector2i RTSize(const rashader::ExecutionPlanner::RT& rt) const;
	/// Updates `user_params()` to be the default values plus the given overrides
	void ReloadUserParams(const std::vector<std::pair<std::string, float>>& overrides);
	/// Writes all constant ubo/push data to the given buffer
	void WriteConstantRAUniforms(u8* buffer, const rashader::BoundBuffer& meta, const std::array<GSVector4, 4>& mvp) const;
	/// Writes all non-constant ubo/push data to the given buffer
	bool WriteRAUniforms(u8* buffer, size_t idx, const rashader::BoundBuffer& meta, const UniformInfo& uniform_info) const;
	/// For generating ubo/push mappings
	static void AddUniform(u32& offset, u32& out, u32 size);
};

MARK_ENUM_AS_FLAGS(GSRAFilterChain::UniformFlags);

GSTexture::Format MapRAShaderFormat(rashader::TextureFormat format);
const char* RAWrapDesc(rashader::WrapMode mode);

struct GSRAPipelineCompiler;

/// Load RAFilterChains in the background
class GSRAFilterChainLoader
{
public:
	enum class State
	{
		InitialLoad,  ///< Loading and parsing slangp
		SPIRVCompile, ///< Compiling slang glsl to SPIR-V
		DataLoaded,   ///< SPIR-V has been parsed, awaiting GS thread check / resource initialize
		MainCompile,  ///< Compiling pipelines and loading textures
		Complete,     ///< Done
		Shutdown,     ///< Loader has been destroyed
	};
	struct Stats
	{
		State state;
		u32 textures_loaded;
		u32 shaders_compiled;
		u32 pipelines_compiled;
		u32 compiles_started;
		u32 total_textures;
		u32 total_shaders;
		u32 total_pipelines;
	};
	class CompileFeedback
	{
		friend class GSRAFilterChainLoader;
		GSRAFilterChainLoader* loader;
		bool wait_destroy = false;
		CompileFeedback(CompileFeedback&&) = delete;
		CompileFeedback(GSRAFilterChainLoader* l);
		~CompileFeedback() { EndWaitDestroy(); }
	public:
		GSRAFilterChainLoader& Loader() { return *loader; }
		bool BeginWaitDestroy();
		void EndWaitDestroy();
		/// Increment the shaders compiled stat and return whether the compile should continue
		bool IncShaders(const GSRAFilterChain& current);
	};
	class SwapCheck;

	static size_t GetNumCompileThreads();
	GSRAFilterChainLoader(GSDevice* dev);

protected:
	virtual ~GSRAFilterChainLoader();

private:
	static void CompileThreadMain(GSRAFilterChainLoader* self, std::unique_ptr<GSRAPipelineCompiler> compiler);
	static void TextureLoadThreadMain(GSRAFilterChainLoader* self);

	std::atomic<u32> m_refcnt{1};
	std::atomic<u32> m_wait_destroy{0};
	u8 m_current_upload_buffer_idx = 0;
	u8 m_threads_on_current = 0;
	u8 m_total_threads = 0;
public:
	/// If true, this filter chain loader has additional work done to it by GSDevice, and therefore will manage updating compile statistics on its own in CheckDone.
	bool m_gsdevice_assisted_compile = false;
	/// If true, this filter chain has a separate counter for its shader compiles
	bool m_separate_shader_compile = true;
protected:
	bool m_has_non_fatal_errors = false;
	bool m_needs_dummy = false;
	bool m_failed = false;
	/// State of compilation
	Stats m_stats = {};
	std::mutex m_mtx;
	std::condition_variable m_cv;
private:
	std::vector<std::thread> m_threads;
	GSRAFilterChain* m_current = nullptr;

	struct TextureInfo {
		u32 index;
		GSVector2i size;
		GSTexture::Format format;
		u16 nmips;
		u16 first_row;
		u16 num_rows;
		u16 mip_level;
		u32 pitch;
		u32 data_offset;
	};
	void* m_current_upload_buffer;
	std::vector<TextureInfo> m_texture_uploads;
	std::vector<TextureInfo> m_texture_uploads_gs_thread;
	std::vector<GSRAFilterChain*> m_discarded_filter_chains;
	GSUploadBuffer* m_upload_buffers[GSUploadBuffer::DELAY_UNTIL_REUSE];

protected:
	void CheckMainCompletion();

private:
	bool ParseSlangP(GSRAFilterChain* chain);
	bool ParseSPIRV(GSRAFilterChain* chain);
	void UploadTextures(GSRAFilterChain* chain, GSDevice* dev);
	void BeginWaitDestroyLocked();

public:
	/// Release the loader (must be empty)
	void Release();
	/// Check if the loader can be released
	bool IsEmpty() const { return m_current && m_discarded_filter_chains.empty(); }
	/// Get the currently loading filter chain
	GSRAFilterChain* GetCurrent() const { return m_current; }
	/// Check on things (call once per frame)
	virtual bool CheckDone(GSDevice* dev);
	/// Retrieve the current filter chain and destroy the instance.  Gives ownership of the filter chain back to the caller.
	GSRAFilterChain* Finish(GSDevice* dev, bool wait = true);
	/// Check if the filter chain was loaded successfully
	bool WasSuccessful() const { return !m_failed; }
	/// Cancel work on the current filter chain and start work on a new one
	void BeginLoad(GSRAFilterChain* chain);
	/// Get status on how the load is going
	Stats GetStats();
	/// Prevent the global GSDevice and any filter chains from being destroyed until a call to `EndWaitDestroy`
	/// If it returns false, the global GSDevice has already been destroyed and you must not attempt to access it or any filter chains on this loader.
	bool BeginWaitDestroy();
	/// Release the assertion started by `BeginWaitDestroy`
	void EndWaitDestroy();
};

struct GSRAPipelineCompiler
{
	// Both the destructor and Compile may be called after the global GSDevice has been destroyed!
	virtual ~GSRAPipelineCompiler() = default;
	/// Compile a pipeline.
	/// This is entered with a WaitDestroy assertion on the loader.
	/// If your compiler can function with the global GSDevice destroyed, copy what you need out of the filter chain, `EndWaitDestroy()`, and then `BeginWaitDestroy` shortly before returning.
	virtual GSRAPipeline* Compile(GSRAFilterChainLoader::CompileFeedback& feedback, const GSRAFilterChain& chain, size_t pass) = 0;
};
