// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GSDevice.h"
#include "GSTexture.h"

#include "common/EnumOps.h"

#include <rashader-lite.h>

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
	/// Removes constant and unused buffer parameters from passes for faster updates
	void CleanConstantBufferParams();
	/// Get the a texture for the given parameters.  Replaces null values with `dummy_tex`.
	GSTexture* GetTexture(rashader::TextureSource source, u32 idx, u32 history) const;
	/// Get the size that the given RT should be resized to for this pass
	GSVector2i RTSize(const rashader::ExecutionPlanner::RT& rt) const;
	/// Updates `user_params()` to be the default values plus the given overrides
	void ReloadUserParams(const std::vector<std::pair<std::string, float>> overrides);
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
