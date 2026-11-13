// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSRAShader.h"

#include "GSRenderer.h"
#include "GS/GSSPIRVCompiler.h"
#include "GS/LibrashaderParams.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "Host.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"
#include "common/Threading.h"

enum : u32
{
	UPLOAD_BUFFER_SIZE = 4 * 1024 * 1024,
};

void GSRAFilterChain::AddUniform(u32& offset, u32& out, u32 size)
{
	if (size)
	{
		out = offset;
		offset += Common::AlignUpPow2(size, 16);
	}
	else
	{
		out = UINT32_MAX;
	}
}

struct MultiArrayAssign
{
	void* ptr;
	size_t current_align = 16;
	MultiArrayAssign(void* ptr_): ptr(ptr_) {}

	template <typename T>
	void assign(size_t count, T*& target)
	{
		pxAssert(current_align >= alignof(T));
		current_align = alignof(T);
		target = reinterpret_cast<T*>(ptr);
		ptr = target + count;
	}
};

static GSRAFilterChain::UniformFlags GetFlags(const rashader::BufferElement& elem, const rashader::FilterChain& chain);

static GSRAFilterChain::UniformFlags GetFlags(const rashader::BoundBuffer& buffer, const rashader::FilterChain& chain)
{
	GSRAFilterChain::UniformFlags flags = GSRAFilterChain::UniformFlags::None;
	for (const rashader::BufferElement& elem : buffer.contents)
		flags |= GetFlags(elem, chain);
	return flags;
}

void GSRAFilterChain::Init()
{
	size_t total_size = 0;
	size_t npasses = info.passes.size();
	size_t nparams = info.parameters.size();
	size_t ntex    = info.textures.size();
	size_t nrt     = plan.num_rts();
	size_t ninput  = plan.input_history_needed() + 1;
	total_size += npasses * sizeof(*passes_);
	total_size += ntex    * sizeof(*textures_);
	total_size += nrt     * sizeof(*rts_);
	total_size += ninput  * sizeof(*input_history_);
	total_size += nparams * sizeof(*user_params_);
	total_size += npasses * sizeof(*uniform_offsets_);

	void* alloc = calloc(1, total_size);
	MultiArrayAssign ptr(alloc);
	ptr.assign(npasses, passes_);
	ptr.assign(ntex,    textures_);
	ptr.assign(nrt,     rts_);
	ptr.assign(ninput,  input_history_);
	ptr.assign(nparams, user_params_);
	ptr.assign(npasses, uniform_offsets_);
	pxAssert(reinterpret_cast<u8*>(alloc) + total_size == ptr.ptr);

	u32 uniform_offset = 0;
	for (size_t i = 0; i < npasses; i++)
		AddUniform(uniform_offset, uniform_offsets()[i].ubo, info.passes[i].ubo.size);
	for (size_t i = 0; i < npasses; i++)
		AddUniform(uniform_offset, uniform_offsets()[i].push, info.passes[i].push.size);
	uniform_data = static_cast<u8*>(calloc(1, uniform_offset));
	for (size_t i = 0; i < npasses; i++)
	{
		const rashader::Pass& pass = info.passes[i];
		UniformMap& map = uniform_offsets()[i];
		map.flags_ubo  = GetFlags(pass.ubo,  info);
		map.flags_push = GetFlags(pass.push, info);
	}

	// User param lookup
	for (size_t i = 0; i < nparams; i++)
		user_param_names.emplace(std::pair<std::string_view, u32>(info.parameters[i].name, static_cast<u32>(i)));
}

GSRAFilterChain::~GSRAFilterChain() = default;

template <typename T>
void DeleteRange(T** begin, T** end, void (*destruct)(T*))
{
	for (; begin < end; begin++)
		if (*begin)
			destruct(*begin);
}

void GSRAFilterChain::Destroy(void (*pipeline_destruct)(GSRAPipeline*), void (*texture_destruct)(GSTexture*))
{
	if (dummy_tex)
		texture_destruct(dummy_tex);
	if (uniform_data)
		free(uniform_data);
	if (!passes_)
		return;
	DeleteRange(passes_, reinterpret_cast<GSRAPipeline**>(textures_), pipeline_destruct);
	DeleteRange(textures_, reinterpret_cast<GSTexture**>(user_params_), texture_destruct);
	free(passes_);
}

void GSRAFilterChain::FinalizeUniformBuffers()
{
	ReloadUserParams(GSConfig.LibrashaderPresetParams);

	size_t npasses = info.passes.size();
	// Do this last so that backends can reorganize the uniform mapping in FinalizeRAFilterChain if they like
	u8* uniform_base = uniform_data;
	for (size_t i = 0; i < npasses; i++)
	{
		static constexpr std::array<GSVector4, 4> mvp_main = {{
			GSVector4::cxpr(1.f, 0.f, 0.f, 0.f),
			GSVector4::cxpr(0.f, 1.f, 0.f, 0.f),
			GSVector4::cxpr(0.f, 0.f, 1.f, 0.f),
			GSVector4::cxpr(0.f, 0.f, 0.f, 1.f),
		}};
		static constexpr std::array<GSVector4, 4> mvp_final = {{
			// Matrices are column-major!
			GSVector4::cxpr( 2.f,  0.f, 0.f, 0.f),
			GSVector4::cxpr( 0.f,  2.f, 0.f, 0.f),
			GSVector4::cxpr( 0.f,  0.f, 1.f, 0.f),
			GSVector4::cxpr(-1.f, -1.f, 0.f, 1.f),
		}};
		const std::array<GSVector4, 4>& mvp = i == npasses - 1 ? mvp_final : mvp_main;
		const GSRAFilterChain::UniformMap& map = uniform_offsets()[i];
		const rashader::Pass& pass = info.passes[i];
		WriteConstantRAUniforms(uniform_base + map.ubo,  pass.ubo,  mvp);
		WriteConstantRAUniforms(uniform_base + map.push, pass.push, mvp);
	}

	// We've uploaded constant buffers, remove them so we don't have to look through anymore
	CleanConstantBufferParams();
}

static void CleanConstantBufferParams(std::vector<rashader::BufferElement>* buffer, const rashader::FilterChain& chain)
{
	auto it = std::remove_if(buffer->begin(), buffer->end(), [&](const rashader::BufferElement& elem) {
		return elem.stages == 0 || GetFlags(elem, chain) == GSRAFilterChain::UniformFlags::None;
	});
	buffer->erase(it, buffer->end());
	buffer->shrink_to_fit();
}

void GSRAFilterChain::CleanConstantBufferParams()
{
	for (rashader::Pass& pass : info.passes)
	{
		::CleanConstantBufferParams(&pass.ubo.contents,  info);
		::CleanConstantBufferParams(&pass.push.contents, info);
	}
}

GSTexture* GSRAFilterChain::GetTexture(rashader::TextureSource source, u32 idx, u32 history) const
{
	GSTexture* out = nullptr;
	switch (source) {
		case rashader::TextureSource::User:
			out = textures()[idx];
			break;
		case rashader::TextureSource::Pass:
			if (!history || plan.frame_no())
				out = rts()[plan.get_pass_output(idx, history)];
			break;
		case rashader::TextureSource::Input:
			if (history > plan.frame_no())
				history = static_cast<u32>(plan.frame_no());
			u32 current = current_input;
			if (current < history)
				current += plan.input_history_needed() + 1;
			return input_history()[current - history];
	}
	if (!out)
		return dummy_tex;
	return out;
}

static float GetScaleBase(rashader::ScaleType type, float size_input, float size_output)
{
	switch (type)
	{
		case rashader::ScaleType::Input:    return size_input;
		case rashader::ScaleType::Viewport: return size_output;
		case rashader::ScaleType::Absolute: return 1;
	}
}

GSVector2i GSRAFilterChain::RTSize(const rashader::ExecutionPlanner::RT& rt) const
{
	float x = GetScaleBase(rt.scale_type_x, size_input.x, size_output.x);
	float y = GetScaleBase(rt.scale_type_y, size_input.y, size_output.y);
	GSVector4i res = GSVector4i(GSVector4(x, y) * GSVector4(rt.scale_x, rt.scale_y)).max_i32(GSVector4i(1, 1));
	return GSVector2i(res.x, res.y);
}

void GSRAFilterChain::ReloadUserParams(const std::vector<std::pair<std::string, float>>& overrides)
{
	for (size_t i = 0; i < info.parameters.size(); i++)
		user_params()[i] = info.parameters[i].value;
	for (const std::pair<std::string, float>& over : overrides)
	{
		auto it = user_param_names.find(over.first);
		if (it != user_param_names.end())
			user_params()[it->second] = over.second;
	}
}

static GSRAFilterChain::UniformFlags GetFlags(rashader::ScaleType scale)
{
	switch (scale)
	{
		case rashader::ScaleType::Input:    return GSRAFilterChain::UniformFlags::HasInputSize;
		case rashader::ScaleType::Viewport: return GSRAFilterChain::UniformFlags::HasOutputSize;
		case rashader::ScaleType::Absolute: return GSRAFilterChain::UniformFlags::None;
	}
	pxAssert(0);
	return GSRAFilterChain::UniformFlags::None;
}

static GSRAFilterChain::UniformFlags GetFlags(const rashader::BufferElement& elem, const rashader::FilterChain& chain)
{
	if (elem.type == rashader::BufferElementType::PassOutputSize)
	{
		const rashader::Pass& pass = chain.passes[elem.index];
		GSRAFilterChain::UniformFlags x = GetFlags(pass.scale_type_x);
		GSRAFilterChain::UniformFlags y = GetFlags(pass.scale_type_y);
		return x | y | GSRAFilterChain::UniformFlags::HasRTSize;
	}
	switch (elem.type)
	{
		case rashader::BufferElementType::FrameCount:
		case rashader::BufferElementType::FrameTimeDelta:
			return GSRAFilterChain::UniformFlags::AlwaysDirty;

		case rashader::BufferElementType::FinalViewportSize:
			return GSRAFilterChain::UniformFlags::HasOutputSize;

		case rashader::BufferElementType::OriginalFPS:
			return GSRAFilterChain::UniformFlags::HasFPS;

		case rashader::BufferElementType::UserParameter:
			return GSRAFilterChain::UniformFlags::HasUserParam;

		case rashader::BufferElementType::OriginalAspect:
		case rashader::BufferElementType::OriginalAspectRotated:
		case rashader::BufferElementType::InputSize:
			return GSRAFilterChain::UniformFlags::HasInputSize;

		case rashader::BufferElementType::PassOutputSize:
			// Handled specially above
			return GSRAFilterChain::UniformFlags::None;

		case rashader::BufferElementType::MVP:
		case rashader::BufferElementType::Rotation:
		case rashader::BufferElementType::FrameDirection:
		case rashader::BufferElementType::CurrentSubFrame:
		case rashader::BufferElementType::TotalSubFrames:
		case rashader::BufferElementType::HDRMode:
		case rashader::BufferElementType::BrightnessNits:
		case rashader::BufferElementType::ExpandGamut:
		case rashader::BufferElementType::TextureSize:
			// Constant
			return GSRAFilterChain::UniformFlags::None;

		case rashader::BufferElementType::Gyroscope:
		case rashader::BufferElementType::Accelerometer:
		case rashader::BufferElementType::AccelerometerRest:
			// Currently Unsupported
			return GSRAFilterChain::UniformFlags::None;
	}
	pxAssert(0);
	return GSRAFilterChain::UniformFlags::None;
}

template <typename T>
static bool WriteUniform(u8* buffer, T data)
{
	bool res = 0 == memcmp(buffer, &data, sizeof(data));
	*reinterpret_cast<T*>(buffer) = data;
	return res;
}

static bool WriteTextureSizeUniform(u8* buffer, GSVector2 size)
{
	GSVector4 data = GSVector4(size).xyxy();
	data = data.xyzw(GSVector4(1.f) / data);
	return WriteUniform(buffer, data);
}

static bool WriteTextureSizeUniform(u8* buffer, GSVector2i size)
{
	GSVector4 data = GSVector4(GSVector4i(size).xyxy());
	data = data.xyzw(GSVector4(1.f) / data);
	return WriteUniform(buffer, data);
}

void GSRAFilterChain::WriteConstantRAUniforms(u8* buffer, const rashader::BoundBuffer& meta, const std::array<GSVector4, 4>& mvp) const
{
	for (const rashader::BufferElement& elem : meta.contents)
	{
		u8* write = buffer + elem.offset;
		switch (elem.type)
		{
			case rashader::BufferElementType::MVP:
				*reinterpret_cast<std::array<GSVector4, 4>*>(write) = mvp;
				break;
			case rashader::BufferElementType::Rotation:
				*reinterpret_cast<u32*>(write) = 0;
				break;
			case rashader::BufferElementType::FrameDirection:
				*reinterpret_cast<s32*>(write) = 1;
				break;
			case rashader::BufferElementType::CurrentSubFrame:
				*reinterpret_cast<u32*>(write) = 1;
				break;
			case rashader::BufferElementType::TotalSubFrames:
				*reinterpret_cast<u32*>(write) = 1;
				break;
			case rashader::BufferElementType::HDRMode:
				*reinterpret_cast<u32*>(write) = 0;
				break;
			case rashader::BufferElementType::BrightnessNits:
				*reinterpret_cast<float*>(write) = 100;
				break;
			case rashader::BufferElementType::ExpandGamut:
				*reinterpret_cast<u32*>(write) = 0;
				break;
			case rashader::BufferElementType::TextureSize:
				WriteTextureSizeUniform(write, GetTexture(rashader::TextureSource::User, elem.index, 0)->GetSize());
				break;

			case rashader::BufferElementType::FinalViewportSize:
			case rashader::BufferElementType::FrameCount:
			case rashader::BufferElementType::OriginalAspect:
			case rashader::BufferElementType::OriginalAspectRotated:
			case rashader::BufferElementType::OriginalFPS:
			case rashader::BufferElementType::FrameTimeDelta:
			case rashader::BufferElementType::UserParameter:
			case rashader::BufferElementType::InputSize:
			case rashader::BufferElementType::PassOutputSize:
			case rashader::BufferElementType::Gyroscope:
			case rashader::BufferElementType::Accelerometer:
			case rashader::BufferElementType::AccelerometerRest:
				// Non-constant
				break;
		}
	}
}

bool GSRAFilterChain::WriteRAUniforms(u8* buffer, size_t idx, const rashader::BoundBuffer& meta, const UniformInfo& uniform_info) const
{
	if (meta.size == 0)
		return false;
	const rashader::Pass& pass = info.passes[idx];
	bool changed = plan.frame_no() == 0; // Need to upload constants on first run
	for (const rashader::BufferElement& elem : meta.contents)
	{
		u8* write = buffer + elem.offset;
		switch (elem.type)
		{
			case rashader::BufferElementType::FinalViewportSize:
				changed |= WriteTextureSizeUniform(write, size_output);
				break;
			case rashader::BufferElementType::FrameCount:
				if (pass.frame_count_mod)
					*reinterpret_cast<u32*>(write) = plan.frame_no() % pass.frame_count_mod;
				else
					*reinterpret_cast<u32*>(write) = static_cast<u32>(plan.frame_no());
				changed = true;
				break;
			case rashader::BufferElementType::OriginalAspect:
			case rashader::BufferElementType::OriginalAspectRotated:
				// TODO: Non-square pixels?
				changed |= WriteUniform(write, size_input.x / size_input.y);
				break;
			case rashader::BufferElementType::OriginalFPS:
				changed |= WriteUniform(write, g_gs_renderer ? static_cast<float>(g_gs_renderer->GetTvRefreshRate()) : 60.f);
				break;
			case rashader::BufferElementType::FrameTimeDelta:
				*reinterpret_cast<u32*>(write) = uniform_info.frame_time_delta;
				changed = true; // No way we're so consistent that each frame takes the same number of µs
				break;
			case rashader::BufferElementType::UserParameter:
				changed |= WriteUniform(write, user_params()[elem.index]);
				break;
			case rashader::BufferElementType::InputSize:
				changed |= WriteTextureSizeUniform(write, GetTexture(rashader::TextureSource::Input, elem.index, elem.history)->GetSize());
				break;
			case rashader::BufferElementType::PassOutputSize:
				if ((elem.index >= idx || elem.history) && plan.frame_no() == 0)
				{
					// No texture will be available, dummy will be used
					changed |= WriteUniform(write, GSVector4(1));
				}
				else if (elem.index <= idx && std::min(elem.history, plan.pass(elem.index).num_history_needed) == 0)
				{
					// Reading an RT that will be resized before use
					u32 idx = plan.get_pass_output(elem.index, elem.history);
					changed |= WriteTextureSizeUniform(write, RTSize(plan.rt(idx)));
				}
				else
				{
					changed |= WriteTextureSizeUniform(write, GetTexture(rashader::TextureSource::Pass, elem.index, elem.history)->GetSize());
				}
				break;
			case rashader::BufferElementType::MVP:
			case rashader::BufferElementType::Rotation:
			case rashader::BufferElementType::FrameDirection:
			case rashader::BufferElementType::CurrentSubFrame:
			case rashader::BufferElementType::TotalSubFrames:
			case rashader::BufferElementType::HDRMode:
			case rashader::BufferElementType::BrightnessNits:
			case rashader::BufferElementType::ExpandGamut:
			case rashader::BufferElementType::TextureSize:
				// Constant
				break;
			case rashader::BufferElementType::Gyroscope:
			case rashader::BufferElementType::Accelerometer:
			case rashader::BufferElementType::AccelerometerRest:
				// Not supported, always zero
				break;
		}
	}
	return changed;
}

GSTexture::Format MapRAShaderFormat(rashader::TextureFormat format)
{
	switch (format)
	{
		case rashader::TextureFormat::R8_UNORM:                 return GSTexture::Format::R8UNorm;
		case rashader::TextureFormat::R8_UINT:                  return GSTexture::Format::R8UInt;
		case rashader::TextureFormat::R8_SINT:                  return GSTexture::Format::R8SInt;
		case rashader::TextureFormat::R8G8_UNORM:               return GSTexture::Format::RG8UNorm;
		case rashader::TextureFormat::R8G8_UINT:                return GSTexture::Format::RG8UInt;
		case rashader::TextureFormat::R8G8_SINT:                return GSTexture::Format::RG8SInt;
		case rashader::TextureFormat::R8G8B8A8_UNORM:           return GSTexture::Format::RGBA8UNorm;
		case rashader::TextureFormat::R8G8B8A8_UINT:            return GSTexture::Format::RGBA8UInt;
		case rashader::TextureFormat::R8G8B8A8_SINT:            return GSTexture::Format::RGBA8SInt;
		case rashader::TextureFormat::R8G8B8A8_SRGB:            return GSTexture::Format::RGBA8SRGB;
		case rashader::TextureFormat::A2B10G10R10_UNORM_PACK32: return GSTexture::Format::RGB10A2UNorm;
		case rashader::TextureFormat::A2B10G10R10_UINT_PACK32:  return GSTexture::Format::RGB10A2UInt;
		case rashader::TextureFormat::R16_UINT:                 return GSTexture::Format::R16UInt;
		case rashader::TextureFormat::R16_SINT:                 return GSTexture::Format::R16SInt;
		case rashader::TextureFormat::R16_SFLOAT:               return GSTexture::Format::R16Float;
		case rashader::TextureFormat::R16G16_UINT:              return GSTexture::Format::RG16UInt;
		case rashader::TextureFormat::R16G16_SINT:              return GSTexture::Format::RG16SInt;
		case rashader::TextureFormat::R16G16_SFLOAT:            return GSTexture::Format::RG16Float;
		case rashader::TextureFormat::R16G16B16A16_UINT:        return GSTexture::Format::RGBA16UInt;
		case rashader::TextureFormat::R16G16B16A16_SINT:        return GSTexture::Format::RGBA16SInt;
		case rashader::TextureFormat::R16G16B16A16_SFLOAT:      return GSTexture::Format::RGBA16Float;
		case rashader::TextureFormat::R32_UINT:                 return GSTexture::Format::R32UInt;
		case rashader::TextureFormat::R32_SINT:                 return GSTexture::Format::R32SInt;
		case rashader::TextureFormat::R32_SFLOAT:               return GSTexture::Format::R32Float;
		case rashader::TextureFormat::R32G32_UINT:              return GSTexture::Format::RG32UInt;
		case rashader::TextureFormat::R32G32_SINT:              return GSTexture::Format::RG32SInt;
		case rashader::TextureFormat::R32G32_SFLOAT:            return GSTexture::Format::RG32Float;
		case rashader::TextureFormat::R32G32B32A32_UINT:        return GSTexture::Format::RGBA32UInt;
		case rashader::TextureFormat::R32G32B32A32_SINT:        return GSTexture::Format::RGBA32SInt;
		case rashader::TextureFormat::R32G32B32A32_SFLOAT:      return GSTexture::Format::RGBA32Float;
	}
	pxAssert(0);
	return GSTexture::Format::Invalid;
}

const char* RAWrapDesc(rashader::WrapMode mode)
{
	switch (mode)
	{
		case rashader::WrapMode::ClampToEdge:    return "Edge";
		case rashader::WrapMode::ClampToBorder:  return "Zero";
		case rashader::WrapMode::Repeat:         return "Repeat";
		case rashader::WrapMode::MirroredRepeat: return "MirroredRepeat";
	}
	pxAssert(0);
	return "Unknown";
}

// Some annoying things that make the async loader more complicated than ideal:
// - GSDeviceVK currently does not refcount its global device, so a background compile thread must be stopped before you can destroy the GSDevice.
// - GSDeviceOGL requires all objects to be created and destroyed on its thread because OpenGL.
// This means that we can't just orphan the texture loader if we don't want it anymore, we need to ensure that all
// objects make their way back to the GS thread to be destroyed.
// Ideally, we don't block the GS thread when we don't need to, e.g. on not-Vulkan destruction.
// To handle this, use a wait-destroy counter.  Whenever a thread is doing something that needs associated GSDevice to be alive,
// it increments the counter until it's done.  If the GSDevice tries to destruct, it will block in the loader destructor until the counter reaches zero.
//
// We also don't want to block if the user changes their mind and selects a different filter chain.
// To handle this, we make sure never to destroy old filter chains while a worker thread might be using them.
// If the filter chain is swapped, the old one is dumped in a garbage pile, and not destroyed until every worker thread has incremented
// `m_threads_on_current` to indicate that it has switched to the latest filter chain.
// Use the `SwapCheck` class to help with this.

static constexpr u32 WAIT_DESTROY_FLAG = 1 << 31;

GSRAFilterChainLoader::CompileFeedback::CompileFeedback(GSRAFilterChainLoader* l): loader(l)
{
	l->BeginWaitDestroyLocked();
	wait_destroy = true;
}

bool GSRAFilterChainLoader::CompileFeedback::BeginWaitDestroy()
{
	if (!wait_destroy)
		wait_destroy = loader->BeginWaitDestroy();
	return wait_destroy;
}

void GSRAFilterChainLoader::CompileFeedback::EndWaitDestroy()
{
	if (wait_destroy)
	{
		loader->EndWaitDestroy();
		wait_destroy = false;
	}
}

bool GSRAFilterChainLoader::CompileFeedback::IncShaders(const GSRAFilterChain& current)
{
	std::lock_guard<std::mutex> lock(loader->m_mtx);
	if (&current != loader->m_current)
		return false;
	loader->m_stats.shaders_compiled++;
	return true;
}

/// Use to verify that the currently-loading filter chain hasn't been swapped when you weren't looking
class GSRAFilterChainLoader::SwapCheck
{
	GSRAFilterChainLoader* loader;
	GSRAFilterChain* current;
public:
	explicit SwapCheck(GSRAFilterChainLoader* l): loader(l), current(l->m_current)
	{
		l->m_threads_on_current++;
	}
	// Reload the current pointer in case it's been swapped
	void refresh()
	{
		if (!is_current())
		{
			current = loader->m_current;
			loader->m_threads_on_current++;
		}
	}
	GSRAFilterChain* operator->() { return current; }
	GSRAFilterChain* get() { return current; }
	bool is_current() const { return loader->m_current == current; }
};

static bool TryLoadTexture(const std::string& path, GSTextureReplacements::ReplacementTexture* data, bool only_base_image)
{
	const char* cpath = path.c_str();
	std::string_view svpath = path;
	GSTextureReplacements::ReplacementTextureLoader loader = GSTextureReplacements::GetLoader(svpath);
	if (!loader)
	{
		Console.ErrorFmt("Unsupported postprocessing texture format {}", svpath);
		return false;
	}
	FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(cpath, "rb");
	GSTextureReplacements::CFile cfile(fp.get());
	if (!fp || !loader(cfile, cpath, data, only_base_image))
	{
		Console.ErrorFmt("Failed to load postprocessing texture {}", svpath);
		return false;
	}
	return true;
}

static bool TextureThreadHasWork(const GSRAFilterChainLoader::Stats& stats)
{
	switch (stats.state)
	{
		case GSRAFilterChainLoader::State::InitialLoad:
			// Initial load is handled by the texture thread
			return true;
		case GSRAFilterChainLoader::State::SPIRVCompile:
			// This one's just waiting
			return false;
		case GSRAFilterChainLoader::State::DataLoaded: // We don't need GS thread stuff to load textures
		case GSRAFilterChainLoader::State::MainCompile:
			return stats.textures_loaded < stats.total_textures;
		case GSRAFilterChainLoader::State::Complete:
		case GSRAFilterChainLoader::State::Shutdown:
			// All done!
			return false;
	}
	pxAssert(0);
	return false;
}

void GSRAFilterChainLoader::TextureLoadThreadMain(GSRAFilterChainLoader* self)
{
	Threading::SetNameOfCurrentThread("RAShader Texture Load");
	ScopedGuard release_on_exit([self]{ self->Release(); });
	std::unique_lock<std::mutex> lock(self->m_mtx);
	SwapCheck chain(self);
	u32 upload_buffer_offset = 0;
	u8* last_upload_buffer = nullptr;
	while (true)
	{
		chain.refresh();
		if (self->m_stats.state == State::Shutdown)
			return;
		if (!chain.get() || !TextureThreadHasWork(self->m_stats))
		{
			self->m_cv.wait(lock);
			continue;
		}
		if (self->m_stats.state == State::InitialLoad)
		{
			self->BeginWaitDestroyLocked();
			lock.unlock();
			bool ok = self->ParseSlangP(chain.get());
			lock.lock();
			self->EndWaitDestroy();
			if (!chain.is_current())
				continue;
			if (ok)
			{
				self->m_stats.total_shaders = static_cast<u32>(self->m_current->info.passes.size() * 2);
				self->m_stats.state = State::SPIRVCompile;
				self->m_cv.notify_all();
			}
			else
			{
				self->m_stats.state = State::Complete;
			}
		}
		else
		{
			pxAssert(self->m_stats.state == State::MainCompile || self->m_stats.state == State::DataLoaded);
			// Find the next used texture to load
			u32 tex;
			for (tex = self->m_stats.textures_loaded; tex < self->m_stats.total_textures; tex++)
				if (self->m_current->info.textures[tex].used)
					goto found;
			self->m_stats.textures_loaded = tex;
			self->CheckMainCompletion();
			continue; // All used textures loaded
		found:
			bool mipmap = self->m_current->info.textures[tex].mipmap;
			std::string path = self->m_current->info.textures[tex].path;
			lock.unlock();

			// Load texture
			GSTextureReplacements::ReplacementTexture data;
			bool ok = TryLoadTexture(path, &data, !mipmap);

			lock.lock();
			if (!chain.is_current())
				continue;
			if (ok)
			{
				// Copy texture data to upload buffer and submit upload request
				TextureInfo info = {};
				info.index = tex;
				info.size = GSVector2i(data.width, data.height);
				info.format = data.format;
				info.nmips = static_cast<u32>(data.mips.size());
				u32 levels = info.nmips + 1;
				if (info.nmips == 0 && mipmap && !GSTexture::IsCompressedFormat(data.format))
					info.nmips = Common::BitScanReverse32(std::max(data.width, data.height) | 1);
				for (info.mip_level = 0; info.mip_level < levels; info.mip_level++)
				{
					u8* src;
					size_t size;
					info.first_row = 0;
					if (info.mip_level)
					{
						auto& mip = data.mips[info.mip_level - 1];
						info.size = GSVector2i(mip.width, mip.height);
						info.pitch = mip.pitch;
						src = mip.data.data();
						size = mip.data.size();
					}
					else
					{
						info.pitch = data.pitch;
						src = data.data.data();
						size = data.data.size();
					}

					// Keep retrying upload until we succeed or the load gets cancelled
					while (true)
					{
						// We can't have the upload buffer destroyed while we're copying to it
						self->BeginWaitDestroyLocked();
						u8* dst = static_cast<u8*>(self->m_current_upload_buffer);
						lock.unlock();

						// Copy as much as we can to the upload buffer
						if (dst != last_upload_buffer)
							upload_buffer_offset = 0;
						last_upload_buffer = dst;
						u32 remaining = UPLOAD_BUFFER_SIZE - upload_buffer_offset;
						u32 upload_size;
						info.num_rows = info.size.y - info.first_row;
						if (remaining >= size)
						{
							memcpy(dst + upload_buffer_offset, src, size);
							upload_size = static_cast<u32>(size);
						}
						else
						{
							u32 rows = remaining / info.pitch;
							if (info.pitch <= UPLOAD_BUFFER_SIZE / 16)
								rows = Common::AlignDownPow2(rows, 16);
							upload_size = rows * info.pitch;
							info.num_rows = GSTexture::GetCompressedBlockSize(info.format) * rows;
							pxAssert(info.num_rows < info.size.y - info.first_row);
							memcpy(dst + upload_buffer_offset, src, upload_size);
						}
						info.data_offset = upload_buffer_offset;

						lock.lock();
						self->EndWaitDestroy();
						if (!chain.is_current() || (self->m_wait_destroy & WAIT_DESTROY_FLAG))
							goto outer_continue;
						if (self->m_current_upload_buffer != dst)
							continue; // Upload buffer swapped, retry

						if (upload_size)
						{
							// If there was space for some data, schedule it to be uploaded
							upload_buffer_offset += upload_size;
							src += upload_size;
							size -= upload_size;
							self->m_texture_uploads.push_back(info);
							info.first_row += info.num_rows;
						}
						if (!size) // Finished?
							break;
						// If we ran out of space, wait for a new upload buffer
						while (self->m_current_upload_buffer == dst)
						{
							self->m_cv.wait(lock);
							if (!chain.is_current())
								goto outer_continue;
						}
					}
				}

				if (mipmap && data.mips.empty())
				{
					// info.data_offset == UINT32_MAX means generate mips
					info.data_offset = UINT32_MAX;
					self->m_texture_uploads.push_back(info);
				}
			}
			else
			{
				self->m_has_non_fatal_errors = true;
				self->m_needs_dummy = true;
			}
			self->m_stats.textures_loaded = tex + 1;
			self->CheckMainCompletion();
		}
outer_continue:;
	}
}

static bool CompileThreadHasWork(const GSRAFilterChainLoader::Stats& stats)
{
	switch (stats.state)
	{
		case GSRAFilterChainLoader::State::SPIRVCompile:
			return stats.compiles_started < stats.total_shaders;
		case GSRAFilterChainLoader::State::MainCompile:
			return stats.compiles_started < stats.total_pipelines;

		case GSRAFilterChainLoader::State::DataLoaded:
		case GSRAFilterChainLoader::State::InitialLoad:
		case GSRAFilterChainLoader::State::Complete:
		case GSRAFilterChainLoader::State::Shutdown:
			// All done!
			return false;
	}
	pxAssert(0);
	return false;
}

struct ShaderCDestruct
{
	void operator()(shaderc_compiler_t c) { GSSPIRVCompiler::Destroy(c); }
};

void GSRAFilterChainLoader::CompileThreadMain(GSRAFilterChainLoader* self, std::unique_ptr<GSRAPipelineCompiler> compiler)
{
	Threading::SetNameOfCurrentThread("RAShader Compile");
	ScopedGuard release_on_exit([self]{ self->Release(); });
	std::unique_lock<std::mutex> lock(self->m_mtx);
	SwapCheck chain(self);
	std::unique_ptr<struct shaderc_compiler, ShaderCDestruct> spirv_compiler;
	while (true)
	{
		chain.refresh();
		if (self->m_stats.state == State::Shutdown)
			return;
		if (!self->m_current || !CompileThreadHasWork(self->m_stats))
		{
			if (spirv_compiler) [[unlikely]]
			{
				lock.unlock();
				spirv_compiler.reset();
				lock.lock();
				continue;
			}
			self->m_cv.wait(lock);
			continue;
		}
		if (self->m_stats.state == State::SPIRVCompile)
		{
			u32 cur = self->m_stats.compiles_started++;
			u32 n = cur >> 1;
			bool fs = cur & 1;
			rashader::Pass& info = self->m_current->info.passes[n];
			const std::string path = info.path;
			const std::string glsl = fs ? info.fs_glsl : info.vs_glsl;
			lock.unlock();

			if (!spirv_compiler)
				spirv_compiler.reset(GSSPIRVCompiler::Create());
			GSSPIRVCompiler::Type type = fs ? GSSPIRVCompiler::Type::FS : GSSPIRVCompiler::Type::VS;
			// We need debug info for reflection
			auto spirv = GSSPIRVCompiler::Compile(spirv_compiler.get(), type, glsl, GSSPIRVCompiler::DebugLevel::Base);
			if (!spirv)
			{
				const char* type_name = type == GSSPIRVCompiler::Type::VS ? "VS" : "FS";
				Host::AddOSDMessage(fmt::format(TRANSLATE_FS("GS", "Failed to compile {} pass {} {}.\nView console for details."), path, n, type_name), Host::OSD_ERROR_DURATION);
			}

			lock.lock();
			if (!chain.is_current())
				continue;
			self->m_stats.shaders_compiled++;
			if (spirv)
			{
				(fs ? info.fs_spirv : info.vs_spirv) = std::move(*spirv);
				if (self->m_stats.shaders_compiled == self->m_stats.total_shaders)
				{
					self->BeginWaitDestroyLocked();
					lock.unlock();
					bool ok = self->ParseSPIRV(chain.get());
					lock.lock();
					self->EndWaitDestroy();
					if (!chain.is_current())
						continue;
					if (!ok)
					{
						self->m_failed = true;
						self->m_stats.state = State::Complete;
						Host::AddKeyedOSDMessage("RASPIRVFailed",
							fmt::format(TRANSLATE_FS("GS", "Failed to process shaders from slangp {}.\nView console for details."), Path::GetFileName(chain->path)),
							Host::OSD_ERROR_DURATION);
						continue;
					}
					self->m_stats.compiles_started = 0;
					self->m_stats.total_textures = static_cast<u32>(chain->info.textures.size());
					self->m_stats.total_pipelines = static_cast<u32>(chain->info.passes.size());
					self->m_stats.shaders_compiled = 0;
					if (!self->m_separate_shader_compile)
						self->m_stats.total_shaders = 0;
					self->m_stats.state = State::DataLoaded;
					self->m_cv.notify_all();
					continue;
				}
			}
			else
			{
				self->m_failed = true;
				self->m_stats.state = State::Complete;
			}
		}
		else
		{
			pxAssert(self->m_stats.state == State::MainCompile);

			u32 n = self->m_stats.compiles_started++;
			GSRAPipeline* pipeline;
			{
				CompileFeedback feedback(self);
				lock.unlock();
				pipeline = compiler->Compile(feedback, *chain.get(), n);
				if (!pipeline)
					Host::AddKeyedOSDMessage("RAPipelineFailed",
						fmt::format(TRANSLATE_FS("GS", "Failed to compile pipeline for slangp {} pass {}.\nView console for details."), Path::GetFileName(chain->path), n),
						Host::OSD_ERROR_DURATION);
				lock.lock();
				if (pipeline)
					chain->passes()[n] = pipeline;
			}
			if (!chain.is_current())
				continue;
			if (pipeline)
			{
				if (!self->m_gsdevice_assisted_compile)
					self->m_stats.pipelines_compiled++;
				self->CheckMainCompletion();
			}
			else
			{
				self->m_failed = true;
				self->m_stats.state = State::Complete;
			}
		}
	}
}

size_t GSRAFilterChainLoader::GetNumCompileThreads()
{
	if (const char* env = getenv("SHADER_COMPILE_THREADS"))
	{
		if (int num = atoi(env))
			return std::min<size_t>(num, 32);
	}
	// Try to leave some threads for PCSX2 to run without contention
	size_t cpu_threads = std::thread::hardware_concurrency();
	if (cpu_threads <= 2)
		return 1;
	if (cpu_threads <= 4)
		return 2;
	if (cpu_threads <= 8)
		return cpu_threads - 2;
	return std::min<size_t>(cpu_threads - 4, 32);
}

GSRAFilterChainLoader::GSRAFilterChainLoader(GSDevice* dev)
{
	size_t nthreads = GetNumCompileThreads();
	// One texture load thread
	m_total_threads = static_cast<u8>(nthreads + 1);
	m_refcnt.store(m_total_threads + 1, std::memory_order_relaxed); // One per thread plus one for the owner
	for (GSUploadBuffer*& buf : m_upload_buffers)
		buf = dev->CreateUploadBuffer(UPLOAD_BUFFER_SIZE);
	m_current_upload_buffer = m_upload_buffers[0]->Map();
	m_threads.emplace_back(TextureLoadThreadMain, this);

	for (size_t i = 0; i < nthreads; i++)
		m_threads.emplace_back(CompileThreadMain, this, std::unique_ptr<GSRAPipelineCompiler>(dev->CreateRAPipelineCompiler()));
}

GSRAFilterChainLoader::~GSRAFilterChainLoader()
{
	pxAssertMsg(!m_current, "Should have reset on destroy");
	for (GSUploadBuffer* buf [[maybe_unused]] : m_upload_buffers)
		pxAssertMsg(!buf, "Should have reset on destroy");
	for (std::thread& thread : m_threads)
		thread.detach(); // One of these threads is the one running the destructor, so we can't join it
}

void GSRAFilterChainLoader::Release()
{
	if (m_refcnt.fetch_sub(1, std::memory_order_acq_rel) == 1)
		delete this;
}

void GSRAFilterChainLoader::CheckMainCompletion()
{
	if (m_stats.state != State::MainCompile)
		return;
	if (m_stats.textures_loaded < m_stats.total_textures)
		return;
	if (m_stats.pipelines_compiled < m_stats.total_pipelines)
		return;
	m_stats.state = State::Complete;
}

bool GSRAFilterChainLoader::ParseSlangP(GSRAFilterChain* chain)
{
	std::vector<rashader::Error> errors;
	rashader::SlangParseConfig config;
	config.has_sensors = false;
	::ParseSlangP(chain->info, chain->path.c_str(), errors, config);
	if (!errors.empty() || !chain->info.ok())
	{
		if (!chain->info.ok())
			Console.ErrorFmt("Failed to load slangp {}:", chain->path);
		else
			Console.ErrorFmt("Warnings when loading slangp {}:", chain->path);
		LogRAShaderErrors(errors);
		m_has_non_fatal_errors = true;
	}
	if (!chain->info.ok())
	{
		Host::AddOSDMessage(fmt::format(TRANSLATE_FS("GS", "Failed to parse slangp {}.\nView console for details."), chain->path), Host::OSD_ERROR_DURATION);
		return false;
	}
	return true;
}

bool GSRAFilterChainLoader::ParseSPIRV(GSRAFilterChain* chain)
{
	std::vector<rashader::SPIRVError> spirv_errors;
	if (!rashader::parse_spirv(chain->info, &spirv_errors))
	{
		Console.ErrorFmt("Failed to process shaders from slangp {}:", chain->path);
		for (rashader::SPIRVError error : spirv_errors)
		{
			const char* stage = error.stage ? "FS" : "VS";
			if (error.target)
				Console.ErrorFmt("\t{} {}: {}: {}", stage, error.pass, error.message, error.target);
			else
				Console.ErrorFmt("\t{} {}: {}", stage, error.pass, error.message);
		}
		Host::AddOSDMessage(fmt::format(TRANSLATE_FS("GS", "Failed to process shaders from slangp {}.\nView console for details."), chain->path), Host::OSD_ERROR_DURATION);
		return false;
	}

	chain->plan = rashader::ExecutionPlanner(chain->info, false);
	chain->Init();
	size_t npasses = chain->info.passes.size();
	for (size_t i = 0; i < npasses; i++)
	{
		for (const rashader::BoundTexture& binding : chain->info.passes[i].bindings)
			if (binding.source == rashader::TextureSource::Pass && (binding.index >= i || binding.history))
				m_needs_dummy = true;
	}
	return true;
}

void GSRAFilterChainLoader::UploadTextures(GSRAFilterChain* chain, GSDevice* dev)
{
	if (!m_texture_uploads.empty())
	{
		GSUploadBuffer* buf = m_upload_buffers[m_current_upload_buffer_idx];
		buf->Unmap();
		m_current_upload_buffer_idx++;
		if (m_current_upload_buffer_idx >= std::size(m_upload_buffers))
			m_current_upload_buffer_idx = 0;
		m_current_upload_buffer = m_upload_buffers[m_current_upload_buffer_idx]->Map();
		std::swap(m_texture_uploads, m_texture_uploads_gs_thread);
		for (const TextureInfo& info : m_texture_uploads_gs_thread)
		{
			GSTexture** ptex = &chain->textures()[info.index];
			GSTexture* tex = *ptex;
			if (!tex)
			{
				*ptex = tex = dev->CreateSurface(GSTexture::Texture, info.size.x, info.size.y, info.nmips + 1, info.format);
#ifdef PCSX2_DEVBUILD
				tex->SetDebugName(TinyString::from_format("ra:{}", chain->info.textures[info.index].name));
#endif
			}
			u32 last_row = info.first_row + info.num_rows;
			if (info.data_offset != UINT32_MAX)
				buf->CopyToTexture(tex, GSVector4i(0, info.first_row, info.size.x, last_row), info.data_offset, info.pitch, info.mip_level);
			else
				tex->GenerateMipmap();
		}
		m_texture_uploads_gs_thread.clear();
		m_cv.notify_all();
	}
}

bool GSRAFilterChainLoader::CheckDone(GSDevice* dev)
{
	std::lock_guard<std::mutex> lock(m_mtx);
	if (!m_discarded_filter_chains.empty() && m_threads_on_current == m_total_threads)
	{
		for (GSRAFilterChain* chain : m_discarded_filter_chains)
			dev->DestroyRAFilterChain(chain);
		m_discarded_filter_chains.clear();
	}
	switch (m_stats.state)
	{
		case State::InitialLoad:
		case State::SPIRVCompile:
			return false;
		case State::Complete:
			return true;
		case State::Shutdown:
			pxAssert(0);
			return true;
		case State::DataLoaded:
			m_stats.state = State::MainCompile;
			m_cv.notify_all();
			return false;
		case State::MainCompile:
			UploadTextures(m_current, dev);
			return false;
	}
	pxAssert(0);
	return false;
}

GSRAFilterChain* GSRAFilterChainLoader::Finish(GSDevice* dev, bool wait)
{
	std::unique_lock<std::mutex> lock(m_mtx);
	GSRAFilterChain* res = m_current;
	m_current = nullptr;
	m_threads_on_current = 0;
	if (u32 val = m_wait_destroy.load(std::memory_order_relaxed))
	{
		if (val != WAIT_DESTROY_FLAG && !wait)
		{
			if (res)
				m_discarded_filter_chains.push_back(res);
			return nullptr;
		}
		if (!(val & WAIT_DESTROY_FLAG))
			m_wait_destroy.fetch_add(WAIT_DESTROY_FLAG, std::memory_order_relaxed);
		while (m_wait_destroy.load(std::memory_order_relaxed) != WAIT_DESTROY_FLAG)
			m_cv.wait(lock);
	}

	if (res)
	{
		if (m_stats.state == State::Complete)
			UploadTextures(res, dev);

		if (m_needs_dummy)
		{
			GSTexture* dummy = dev->CreateSurface(GSTexture::Texture, 1, 1, 1, GSTexture::Format::Color);
			res->dummy_tex = dummy;
			u32 pixel = 0;
			dummy->Update(GSVector4i::cxpr(0, 0, 1, 1), &pixel, 4);
#ifdef PCSX2_DEVBUILD
			dummy->SetDebugName("ra:dummy");
#endif
		}

		if (m_has_non_fatal_errors)
			Host::AddKeyedOSDMessage("RAShaderWarnings",
				fmt::format(TRANSLATE_FS("GS", "Postprocessing shader {} loaded with warnings.\nView console for details."), Path::GetFileName(res->path)),
				Host::OSD_WARNING_DURATION);
	}

	for (GSUploadBuffer*& buf : m_upload_buffers)
	{
		delete buf;
		buf = nullptr;
	}
	for (GSRAFilterChain* chain : m_discarded_filter_chains)
		dev->DestroyRAFilterChain(chain);
	m_discarded_filter_chains.clear();
	m_stats.state = State::Shutdown;
	m_cv.notify_all();
	return res;
}

void GSRAFilterChainLoader::BeginLoad(GSRAFilterChain* chain)
{
	{
		std::unique_lock<std::mutex> lock(m_mtx);
		if (m_current)
			m_discarded_filter_chains.push_back(m_current);
		m_current = chain;
		m_threads_on_current = 0;
		memset(&m_stats, 0, sizeof(m_stats));
		m_stats.state = State::InitialLoad;
		if (m_wait_destroy.load(std::memory_order_relaxed) & WAIT_DESTROY_FLAG)
			m_wait_destroy.fetch_sub(WAIT_DESTROY_FLAG, std::memory_order_relaxed);
	}
	m_cv.notify_all();
}

GSRAFilterChainLoader::Stats GSRAFilterChainLoader::GetStats()
{
	std::lock_guard<std::mutex> lock(m_mtx);
	return m_stats;
}

void GSRAFilterChainLoader::BeginWaitDestroyLocked()
{
	m_wait_destroy.fetch_add(1, std::memory_order_relaxed);
	pxAssert(m_current);
}

bool GSRAFilterChainLoader::BeginWaitDestroy()
{
	std::lock_guard<std::mutex> lock(m_mtx);
	if (!m_current)
		return false;
	m_wait_destroy.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void GSRAFilterChainLoader::EndWaitDestroy()
{
	u32 val = m_wait_destroy.fetch_sub(1, std::memory_order_relaxed);
	pxAssert(val & ~WAIT_DESTROY_FLAG);
	if (val == WAIT_DESTROY_FLAG + 1)
		m_cv.notify_all();
}
