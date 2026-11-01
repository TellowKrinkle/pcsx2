// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <rashader-lite.h>
#include "GSDevice.h"
#include "GSTexture.h"
#include <span>

struct GSRAPipeline
{
	GSRAPipeline() = default;
	GSRAPipeline(GSRAPipeline&&) = delete;
protected:
	~GSRAPipeline() = default;
};

struct GSRAFilterChain
{
	enum class UniformFlags : u16
	{
		None = 0,
	};

	struct UniformMap
	{
		u32 ubo;  ///< Offset of UBO in `uniform_data`
		u32 push; ///< Offset of push in `uniform_data`
		UniformFlags flags_ubo;
		UniformFlags flags_push;
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

	void Init();
	GSTexture* GetTexture(rashader::TextureSource source, u32 idx, u32 history) const;
	GSVector2i RTSize(const rashader::ExecutionPlanner::RT& rt) const;
	void ReloadUserParams(const std::vector<std::pair<std::string, float>> overrides);
	void WriteConstantRAUniforms(u8* buffer, const rashader::BoundBuffer& meta, const std::array<GSVector4, 4>& mvp) const;
	bool WriteRAUniforms(u8* buffer, size_t idx, const rashader::BoundBuffer& meta, const UniformInfo& uniform_info) const;
};

GSTexture::Format MapRAShaderFormat(rashader::TextureFormat format);
const char* RAWrapDesc(rashader::WrapMode mode);
