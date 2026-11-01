// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GSRAShader.h"

#include "GSRenderer.h"

static void AddUniform(u32& offset, u32& out, u32 size)
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

	void align(u32 n)
	{
		ptr = reinterpret_cast<void*>(Common::AlignUpPow2(reinterpret_cast<uintptr_t>(ptr), n));
		current_align = n;
	}

	template <typename T>
	void assign(size_t count, T*& target)
	{
		pxAssert(current_align >= alignof(T));
		current_align = alignof(T);
		target = reinterpret_cast<T*>(ptr);
		ptr = target + count;
	}
};

void GSRAFilterChain::Init()
{
	size_t uniform_size = 0;
	size_t total_size = 0;
	for (rashader::Pass pass : info.passes)
		uniform_size += Common::AlignUpPow2(pass.ubo.size, 16) + Common::AlignUpPow2(pass.push.size, 16);
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
	total_size = Common::AlignUpPow2(total_size, 16);
	total_size += uniform_size;

	void* alloc = calloc(1, total_size);
	MultiArrayAssign ptr(alloc);
	ptr.assign(npasses, passes_);
	ptr.assign(ntex,    textures_);
	ptr.assign(nrt,     rts_);
	ptr.assign(ninput,  input_history_);
	ptr.assign(nparams, user_params_);
	ptr.assign(npasses, uniform_offsets_);
	ptr.align(16);
	ptr.assign(uniform_size, uniform_data);
	pxAssert(reinterpret_cast<u8*>(alloc) + total_size == ptr.ptr);

	u32 uniform_offset = 0;
	for (size_t i = 0; i < npasses; i++)
		AddUniform(uniform_offset, uniform_offsets()[i].ubo, info.passes[i].ubo.size);
	for (size_t i = 0; i < npasses; i++)
		AddUniform(uniform_offset, uniform_offsets()[i].push, info.passes[i].push.size);
	pxAssert(uniform_offset == uniform_size);

	// User param lookup
	for (size_t i = 0; i < nparams; i++)
		user_param_names.emplace(std::make_pair(info.parameters[i].name, static_cast<u32>(i)));
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
	if (!passes_)
		return;
	DeleteRange(passes_, reinterpret_cast<GSRAPipeline**>(textures_), pipeline_destruct);
	DeleteRange(textures_, reinterpret_cast<GSTexture**>(user_params_), texture_destruct);
	free(passes_);
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

void GSRAFilterChain::ReloadUserParams(const std::vector<std::pair<std::string, float>> overrides)
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
	if (meta.contents.empty())
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
}
