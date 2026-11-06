// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: MIT

#include <rashader-lite.h>
#include "common.h"
#include "spirv.hpp"
#include <algorithm>
#include <array>

#define ENUM_SHADER_PARAMETERS(X) \
	X(MVP) \
	X(FinalViewportSize) \
	X(FrameCount) \
	X(Rotation) \
	X(OriginalAspect) \
	X(OriginalAspectRotated) \
	X(OriginalFPS) \
	X(FrameTimeDelta) \
	X(FrameDirection) \
	X(CurrentSubFrame) \
	X(TotalSubFrames) \
	X(HDRMode) \
	X(BrightnessNits) \
	X(ExpandGamut) \
	X(Gyroscope) \
	X(Accelerometer) \
	X(AccelerometerRest) \

namespace rashader {
enum class KnownTexture { Original, OriginalFeedback, Source, OriginalHistory, PassOutput, PassFeedback, User, Last = User };
enum class KnownParameter {
#define PARAM(x) x,
	ENUM_SHADER_PARAMETERS(PARAM)
#undef PARAM
	Last = AccelerometerRest,
};
};

static constexpr std::string_view KnownTextureName(rashader::KnownTexture tex) {
	switch (tex) {
#define CASE(x) case rashader::KnownTexture::x: return #x
		CASE(Original);
		CASE(OriginalFeedback);
		CASE(Source);
		CASE(OriginalHistory);
		CASE(PassOutput);
		CASE(PassFeedback);
		CASE(User);
#undef CASE
	}
}

static constexpr std::string_view KnownParameterName(rashader::KnownParameter param) {
	switch (param) {
#define CASE(x) case rashader::KnownParameter::x: return #x;
		ENUM_SHADER_PARAMETERS(CASE)
#undef CASE
	}
}

static constexpr std::string_view TextureSourceName(rashader::TextureSource source) {
	switch (source) {
		case rashader::TextureSource::User:  return "User";
		case rashader::TextureSource::Pass:  return "Pass";
		case rashader::TextureSource::Input: return "Input";
	}
}

static constexpr std::string_view BufferElementTypeName(rashader::BufferElementType type) {
	switch (type) {
#define CASE(x) case rashader::BufferElementType::x: return #x;
			ENUM_SHADER_PARAMETERS(CASE)
#undef CASE
		case rashader::BufferElementType::UserParameter:  return "UserParameter";
		case rashader::BufferElementType::InputSize:      return "InputSize";
		case rashader::BufferElementType::PassOutputSize: return "PassOutputSize";
		case rashader::BufferElementType::TextureSize:    return "TextureSize";
	}
}

static rashader::BufferElementType get_buffer_element_type(rashader::KnownParameter param) {
#define CHECK(x) static_assert(static_cast<uint32_t>(rashader::KnownParameter::x) == static_cast<uint32_t>(rashader::BufferElementType::x), "Should match");
	ENUM_SHADER_PARAMETERS(CHECK)
#undef CHECK
	return static_cast<rashader::BufferElementType>(param);
}

#undef ENUM_SHADER_PARAMETERS

static constexpr auto KnownTextureLookup      = rashader::make_enum_lookup<KnownTextureName>();
static constexpr auto KnownParameterLookup    = rashader::make_enum_lookup<KnownParameterName>();
static constexpr auto TextureSourceLookup     = rashader::make_enum_lookup<TextureSourceName>();
static constexpr auto BufferElementTypeLookup = rashader::make_enum_lookup<BufferElementTypeName>();

namespace rashader {

struct Instruction {
	const uint32_t* words;

	constexpr Instruction(std::nullptr_t): words(nullptr) {}
	explicit constexpr Instruction(const uint32_t* words): words(words) {}
	Instruction(const Instruction&) = default;

	uint32_t size() const { return words[0] >> 16; }
	spv::Op op() const { return static_cast<spv::Op>(words[0] & 0xffff); }
	const uint32_t& operator[](std::size_t idx) const {
		return words[idx];
	}
	std::string_view string(std::size_t idx) const {
		const char* beg = reinterpret_cast<const char*>(words + idx);
		const char* end = reinterpret_cast<const char*>(words + size());
		if (beg >= end)
			return {};
		return std::string_view(beg, strnlen(beg, beg - end));
	}
	constexpr operator bool() const { return words; }
	Instruction next() const { return Instruction(words + size()); }
};

struct SPIRVInstructionInfo {
	uint8_t min_size     : 6;
	bool has_result_type : 1;
	bool is_definition   : 1;
};

static SPIRVInstructionInfo get_instruction_info(spv::Op op) {
	static constexpr auto lookup = []{
		std::array<SPIRVInstructionInfo, 101> out = {};
		out[spv::OpName]                = { 3, false, false };
		out[spv::OpMemberName]          = { 4, false, false };
		out[spv::OpDecorate]            = { 3, false, false };
		out[spv::OpMemberDecorate]      = { 4, false, false };
		out[spv::OpVariable]            = { 4, true,  true  };
		out[spv::OpConstant]            = { 3, true,  true  };
		out[spv::OpTypeVoid]            = { 2, false, true  };
		out[spv::OpTypeBool]            = { 2, false, true  };
		out[spv::OpTypeInt]             = { 4, false, true  };
		out[spv::OpTypeFloat]           = { 3, false, true  };
		out[spv::OpTypeVector]          = { 4, false, true  };
		out[spv::OpTypeMatrix]          = { 4, false, true  };
		out[spv::OpTypeImage]           = { 9, false, true  };
		out[spv::OpTypeSampler]         = { 2, false, true  };
		out[spv::OpTypeSampledImage]    = { 3, false, true  };
		out[spv::OpTypeStruct]          = { 2, false, true  };
		out[spv::OpTypeArray]           = { 4, false, true  };
		out[spv::OpTypePointer]         = { 4, false, true  };
		out[spv::OpFunctionCall]        = { 4, true,  true  };
		out[spv::OpFunction]            = { 5, true,  true  };
		out[spv::OpFunctionEnd]         = { 1, false, false };
		out[spv::OpFunctionParameter]   = { 3, true,  true  };
		out[spv::OpEntryPoint]          = { 4, true,  false };
		out[spv::OpAccessChain]         = { 4, true,  true  };
		out[spv::OpInBoundsAccessChain] = { 4, true,  true  };
		out[spv::OpCompositeExtract]    = { 4, true,  true  };
		out[spv::OpLoad]                = { 4, true,  true  };
		out[spv::OpImage]               = { 4, true,  true  };
		out[spv::OpSampledImage]        = { 5, true,  true  };
		return out;
	}();
	return op >= lookup.size() ? SPIRVInstructionInfo{} : lookup[op];
}

struct SPIRVDeclaration {
	Instruction inst = nullptr;
	const char* name;
	bool used        : 1;
	bool has_set     : 1;
	bool has_binding : 1;
	bool is_function : 1;
	uint16_t set;
	uint32_t binding;
};

enum class ShaderType : uint8_t { Unrecognized, Mat4, Vec4, Vec3, Float, UInt, SInt };

static ShaderType get_shader_type(BufferElementType type) {
	switch (type) {
		case BufferElementType::MVP:                   return ShaderType::Mat4;
		case BufferElementType::FinalViewportSize:     return ShaderType::Vec4;
		case BufferElementType::FrameCount:            return ShaderType::UInt;
		case BufferElementType::FrameDirection:        return ShaderType::SInt;
		case BufferElementType::FrameTimeDelta:        return ShaderType::UInt;
		case BufferElementType::OriginalFPS:           return ShaderType::Float;
		case BufferElementType::Rotation:              return ShaderType::UInt;
		case BufferElementType::OriginalAspect:        return ShaderType::Float;
		case BufferElementType::OriginalAspectRotated: return ShaderType::Float;
		case BufferElementType::TotalSubFrames:        return ShaderType::UInt;
		case BufferElementType::CurrentSubFrame:       return ShaderType::UInt;
		case BufferElementType::HDRMode:               return ShaderType::UInt;
		case BufferElementType::BrightnessNits:        return ShaderType::Float;
		case BufferElementType::ExpandGamut:           return ShaderType::UInt;
		case BufferElementType::Gyroscope:             return ShaderType::Vec3;
		case BufferElementType::Accelerometer:         return ShaderType::Vec3;
		case BufferElementType::AccelerometerRest:     return ShaderType::Vec3;
		case BufferElementType::UserParameter:         return ShaderType::Float;
		case BufferElementType::InputSize:             return ShaderType::Vec4;
		case BufferElementType::PassOutputSize:        return ShaderType::Vec4;
		case BufferElementType::TextureSize:           return ShaderType::Vec4;
	}
	return ShaderType::Float;
}

static uint32_t get_size(ShaderType type) {
	switch (type) {
		case ShaderType::Mat4:  return 64;
		case ShaderType::Vec4:  return 16;
		case ShaderType::Vec3:  return 12;
		case ShaderType::Float: return  4;
		case ShaderType::UInt:  return  4;
		case ShaderType::SInt:  return  4;

		case ShaderType::Unrecognized:
			break;
	}
	return 4;
}

struct StructMemberInfo {
	const char* name = nullptr;
	uint32_t offset = UINT32_MAX;
	ShaderType type = ShaderType::Unrecognized;
	bool used = false;
};

struct StructInfo {
	uint32_t spirv_id;
	std::vector<StructMemberInfo> members;
};

static bool try_match_texture_reference(BoundTexture* out, std::string_view tex_name, std::string_view match_name, TextureSource source, uint32_t idx) {
	if (parse::try_consume_prefix(&match_name, tex_name)) {
		if (match_name.empty()) {
			out->source  = source;
			out->index   = idx;
			out->history = 0;
			return true;
		}
		if (match_name == "Feedback" && source == TextureSource::Pass) {
			out->source  = source;
			out->index   = idx;
			out->history = 1;
			return true;
		}
	}
	return false;
}

static bool try_match_texture_reference(BufferElement* out, std::string_view tex_name, std::string_view match_name, BufferElementType type, uint32_t idx) {
	if (parse::try_consume_prefix(&match_name, tex_name)) {
		if (match_name == "Size") {
			out->type    = type;
			out->index   = idx;
			out->history = 0;
			return true;
		}
		if (match_name == "FeedbackSize") {
			out->type    = type;
			out->index   = idx;
			out->history = 1;
			return true;
		}
	}
	return false;
}

static bool get_texture_info(BoundTexture* out, KnownTexture tex, std::optional<uint64_t> idx, uint32_t pass) {
	switch (tex) {
		case KnownTexture::Source:
		case KnownTexture::Original:
		case KnownTexture::OriginalFeedback:
			if (idx)
				return false;
			break;
		case KnownTexture::OriginalHistory:
		case KnownTexture::PassOutput:
		case KnownTexture::PassFeedback:
		case KnownTexture::User:
			if (!idx)
				return false;
			break;
	}
	switch (tex) {
		case KnownTexture::Source:
			if (pass) {
				out->source = TextureSource::Pass;
				out->index = pass - 1;
				out->history = 0;
				break;
			}
			[[fallthrough]];
		case KnownTexture::Original:
		case KnownTexture::OriginalFeedback:
			out->source  = TextureSource::Input;
			out->index   = 0;
			out->history = tex == KnownTexture::OriginalFeedback ? 1 : 0;
			break;
		case KnownTexture::OriginalHistory:
			out->source  = TextureSource::Input;
			out->index   = 0;
			out->history = static_cast<uint32_t>(*idx);
			break;
		case KnownTexture::PassOutput:
		case KnownTexture::PassFeedback:
			out->source  = TextureSource::Pass;
			out->index   = static_cast<uint32_t>(*idx);
			out->history = tex == KnownTexture::PassFeedback ? 1 : 0;
			break;
		case KnownTexture::User:
			out->source  = TextureSource::User;
			out->index   = static_cast<uint32_t>(*idx);
			out->history = 0;
			break;
	}
	return true;
}

class SPIRVParser {
	FilterChain& chain;
	uint32_t pass_no;
	uint32_t stage;
	std::vector<SPIRVError>* errors;
	std::vector<SPIRVDeclaration> decls;
	std::vector<StructInfo> structs;
	uint32_t ubo;
	uint32_t push;
	NameLookup param_lookup;

	NOINLINE void report_error(const char* message, const char* target = nullptr) {
		if (errors)
			errors->emplace_back(pass_no, stage, message, target);
	}

	// Returns the OpVariable that defines the variable being loaded or sampled
	Instruction find_base_variable(uint32_t access) {
		while (true) {
			if (access >= decls.size())
				return nullptr;
			Instruction inst = decls[access].inst;
			if (!inst)
				return nullptr;
			uint32_t offset;
			switch (inst.op()) {
				case spv::OpAccessChain:
				case spv::OpInBoundsAccessChain:
					access = inst[3];
					break;
				case spv::OpVariable:
				case spv::OpFunctionParameter:
					return inst;
				default:
					return nullptr;
			}
		}
	}

	static bool find_texture_compare(const BoundTexture& t, uint32_t binding) {
		return t.binding < binding;
	}

	static bool find_struct_compare(const StructInfo& s, uint32_t id) {
		return s.spirv_id < id;
	}

	StructInfo* find_struct(uint32_t id) {
		auto it = std::lower_bound(structs.begin(), structs.end(), id, find_struct_compare);
		return it == structs.end() || it->spirv_id != id ? nullptr : &*it;
	}

	void handle_variable(Instruction base) {
		if (base.op() == spv::OpFunctionParameter)
			return;
		if (base.op() != spv::OpVariable) {
			report_error("Variable had wrong op");
			return;
		}
		SPIRVDeclaration& type = decls[base[1]];
		SPIRVDeclaration& decl = decls[base[2]];
		switch (static_cast<spv::StorageClass>(base[3])) {
			case spv::StorageClassPushConstant:
				if (push != UINT32_MAX && push != base[2])
					report_error("Multiple push constants", decl.name);
				push = base[2];
				decl.used = true;
				break;
			case spv::StorageClassUniform:
				if (!decl.has_binding) {
					report_error("Load from uniform with no binding", decl.name);
				} else if (decl.set != 0) {
					report_error("Load from uniform that's not at set 0", decl.name);
				} else {
					if (ubo != UINT32_MAX && ubo != base[2])
						report_error("Multiple set 0 binding 0 UBOs");
					if (ubo != UINT32_MAX && decls[ubo].binding != decl.binding)
						report_error("Multiple UBOs at different bindings");
					ubo = base[2];
					decl.used = true;
				}
				break;
			case spv::StorageClassUniformConstant:
				if (!type.inst || type.inst.op() != spv::OpTypePointer) {
					report_error("Uniform constant of non-pointer type", decl.name);
				} else {
					uint32_t pointee = type.inst[3];
					if (pointee >= decls.size() || !decls[pointee].inst) {
						report_error("Uniform constant of pointer to unrecognized type", decl.name);
					} else {
						spv::Op op = decls[pointee].inst.op();
						if (op != spv::OpTypeSampledImage && op != spv::OpTypeImage) {
							report_error("Uniform constant of pointer to non-image type", decl.name);
						} else if (!decl.has_binding) {
							report_error("Used image with no binding", decl.name);
						} else if (decl.set != 0) {
							report_error("Used image that's not in set 0", decl.name);
						} else {
							decl.used = true;
						}
					}
				}
				break;
			case spv::StorageClassPrivate:
			case spv::StorageClassFunction:
			case spv::StorageClassInput:
			case spv::StorageClassOutput:
				break;
			default:
				report_error("Load from unrecognized storage class", decls[base[2]].name);
				break;
		}
	}

	bool handle_function(uint32_t id) {
		decls[id].used = true;
		if (!decls[id].inst) {
			report_error("Calls function without body");
			return false;
		}

		for (Instruction inst = decls[id].inst; inst.op() != spv::OpFunctionEnd; inst = inst.next()) {
			spv::Op op = inst.op();
			switch (op) {
				case spv::OpFunctionCall:
					if (inst[3] >= decls.size() || !decls[inst[3]].is_function) {
						report_error("Calls undefined function", inst[3] >= decls.size() ? nullptr : decls[inst[3]].name);
						return false;
					}
					if (!decls[inst[3]].used) {
						if (!handle_function(inst[3]))
							return false;
					}
					for (uint32_t i = 4; i < inst.size(); i++) {
						if (Instruction arg = find_base_variable(inst[i]))
							handle_variable(arg);
					}
					break;

				case spv::OpLoad:
					if (Instruction base = find_base_variable(inst[3])) {
						handle_variable(base);
					} else {
						report_error("Failed to track load source", decls[inst[2]].name);
						return false;
					}
					break;

				case spv::OpAccessChain:
				case spv::OpInBoundsAccessChain:
					if (inst[3] >= decls.size() || !decls[inst[3]].inst || decls[inst[3]].inst[1] >= decls.size()) {
						report_error("Bad access chain base");
						return false;
					}
					if (inst.size() >= 5 && inst[4] < decls.size() && decls[inst[4]].inst) {
						// Looking for pointer to struct, to mark those struct members as used
						SPIRVDeclaration& first_idx = decls[inst[4]];
						SPIRVDeclaration& ptr = decls[decls[inst[3]].inst[1]];
						if (!ptr.inst || ptr.inst.op() != spv::OpTypePointer) {
							report_error("Access chain through non-pointer");
							return false;
						}
						uint32_t type_id = ptr.inst[3];
						bool first_idx_is_constant = first_idx.inst.op() == spv::OpConstant && first_idx.inst.size() >= 4;
						if (type_id >= decls.size()) {
							report_error("Bad OpTypePointer");
							return false;
						}
						if (decls[type_id].inst && decls[type_id].inst.op() == spv::OpTypeStruct) {
							if (!first_idx_is_constant) {
								report_error("Indexed struct with non-constant index", decls[type_id].name);
								return false;
							}
							uint32_t member_idx = first_idx.inst[3];
							StructInfo* info = find_struct(type_id);
							if (!info || member_idx >= info->members.size()) {
								report_error("Indexed struct out of bounds", decls[type_id].name);
								return false;
							}
							info->members[member_idx].used = true;
						} else if (!decls[type_id].inst) {
							report_error("OpTypePointer to unrecognized type", decls[type_id].name);
						}
					}
					break;

				case spv::OpCompositeExtract:
					if (inst[3] >= decls.size()) {
						report_error("Bad composite extract base");
						return false;
					}
					if (!decls[inst[3]].inst || decls[inst[3]].inst[1] >= decls.size())
						continue;
					if (inst.size() >= 5) {
						// Looking for struct, to mark those struct members as used
						uint32_t type_id = decls[inst[3]].inst[1];
						if (decls[type_id].inst && decls[type_id].inst.op() == spv::OpTypeStruct) {
							uint32_t member_idx = inst[4];
							StructInfo* info = find_struct(type_id);
							if (!info || member_idx >= info->members.size()) {
								report_error("Indexed struct out of bounds", decls[type_id].name);
								return false;
							}
							info->members[member_idx].used = true;
						} else if (!decls[type_id].inst) {
							report_error("OpCompositeExtract on unrecognized type", decls[type_id].name);
						}
					}
					break;

				default:
					break;
			}
		}

		return true;
	}

	bool lookup_texture_reference(BoundTexture* out, std::string_view name) {
		for (size_t i = 0; i < chain.textures.size(); i++) {
			Texture& tex = chain.textures[i];
			std::string_view suffix = name;
			if (try_match_texture_reference(out, tex.name, name, TextureSource::User, static_cast<uint32_t>(i)))
				return true;
		}
		for (size_t i = 0; i < chain.passes.size(); i++) {
			Pass& pass = chain.passes[i];
			if (pass.name.empty())
				continue;
			if (try_match_texture_reference(out, pass.name, name, TextureSource::Pass, static_cast<uint32_t>(i)))
				return true;
		}
		std::optional<uint64_t> idx = parse::parse_uint64_suffix(&name);
		if (idx && *idx > UINT32_MAX)
			return false;
		if (std::optional<KnownTexture> tex = KnownTextureLookup.ToEnum(name))
			return get_texture_info(out, *tex, idx, pass_no);
		return false;
	}

	void handle_texture(const SPIRVDeclaration& decl) {
		BoundTexture tex = {};
		tex.binding = decl.binding;
		if (!decl.name) {
			report_error("Texture missing debug info name (required for reflection)");
			return;
		}
		if (!lookup_texture_reference(&tex, decl.name)) {
			report_error("Unrecognized texture name", decl.name);
			return;
		}
		switch (tex.source) {
			case TextureSource::Input:
				tex.linear = chain.passes[0].filter_linear;
				tex.wrap   = chain.passes[0].wrap;
				break;
			case TextureSource::Pass:
				if (tex.index >= chain.passes.size()) {
					report_error("Pass out of bounds", decl.name);
					return;
				}
				if (!tex.history && tex.index >= pass_no) {
					report_error("Pass references future texture", decl.name);
					return;
				}
				{
					// PassFeedback uses the pass's index, but PassOutput uses the next pass's index.
					size_t index = tex.index + (tex.history ? 0 : 1);
					index = std::min(index, chain.passes.size() - 1);
					tex.linear = chain.passes[index].filter_linear;
					tex.wrap   = chain.passes[index].wrap;
				}
				break;
			case TextureSource::User:
				if (tex.index >= chain.num_explicit_textures) {
					report_error("Texture out of bounds", decl.name);
					return;
				}
				chain.textures[tex.index].used = true;
				tex.linear = chain.textures[tex.index].linear;
				tex.wrap   = chain.textures[tex.index].wrap;
				break;
		}

		std::vector<BoundTexture>& bindings = chain.passes[pass_no].bindings;
		auto iter = std::lower_bound(bindings.begin(), bindings.end(), decl.binding, find_texture_compare);
		if (iter == bindings.end() || iter->binding != decl.binding) {
			iter = bindings.insert(iter, tex);
		} else if (iter->source != tex.source || iter->index != tex.index || iter->history != tex.history) {
			report_error("Texture doesn't match previously seen texture at same binding", decl.name);
		}
		iter->stages |= 1 << stage;
	}

	std::optional<uint32_t> get_float_vector_size(const SPIRVDeclaration& decl) {
		if (decl.inst[2] >= decls.size() || !decls[decl.inst[2]].inst) {
			return std::nullopt;
		}
		const SPIRVDeclaration& elem = decls[decl.inst[2]];
		if (elem.inst.op() != spv::OpTypeFloat || elem.inst[2] != 32) {
			return std::nullopt;
		}
		return decl.inst[3];
	}

	ShaderType get_shader_type(uint32_t type_id) {
		if (type_id >= decls.size() || !decls[type_id].inst) {
			return ShaderType::Unrecognized;
		}
		const SPIRVDeclaration& decl = decls[type_id];
		switch (decl.inst.op()) {
			case spv::OpTypeMatrix:
				if (decl.inst[3] != 4) {
					return ShaderType::Unrecognized;
				}
				if (decl.inst[2] >= decls.size() || !decls[decl.inst[2]].inst || decls[decl.inst[2]].inst.op() != spv::OpTypeVector) {
					return ShaderType::Unrecognized;
				}
				if (std::optional<uint32_t> size = get_float_vector_size(decls[decl.inst[2]])) {
					if (*size == 4)
						return ShaderType::Mat4;
				}
				return ShaderType::Unrecognized;
			case spv::OpTypeVector:
				if (std::optional<uint32_t> size = get_float_vector_size(decl)) {
					if (*size == 3)
						return ShaderType::Vec3;
					if (*size == 4)
						return ShaderType::Vec4;
				}
				return ShaderType::Unrecognized;
			case spv::OpTypeFloat:
				if (decl.inst[2] != 32) {
					return ShaderType::Unrecognized;
				}
				return ShaderType::Float;
			case spv::OpTypeInt:
				if (decl.inst[2] != 32) {
					return ShaderType::Unrecognized;
				}
				return decl.inst[3] ? ShaderType::SInt : ShaderType::UInt;
			default:
				return ShaderType::Unrecognized;
		}
	}

	bool lookup_buffer_member_reference(BufferElement* out, std::string_view name) {
		{
			auto it = param_lookup.find(name);
			if (it != param_lookup.end()) {
				chain.parameters[it->second].used = true;
				out->type    = BufferElementType::UserParameter;
				out->index   = static_cast<uint32_t>(it->second);
				out->history = 0;
				return true;
			}
		}
		if (parse::has_suffix(name, "Size")) {
			for (size_t i = 0; i < chain.textures.size(); i++) {
				Texture& tex = chain.textures[i];
				if (try_match_texture_reference(out, tex.name, name, BufferElementType::TextureSize, static_cast<uint32_t>(i)))
					return true;
			}
			for (size_t i = 0; i < chain.passes.size(); i++) {
				Pass& pass = chain.passes[i];
				if (pass.name.empty())
					continue;
				if (try_match_texture_reference(out, pass.name, name, BufferElementType::PassOutputSize, static_cast<uint32_t>(i)))
					return true;
			}
		}
		std::optional<uint64_t> idx = parse::parse_uint64_suffix(&name);
		if (idx && *idx > UINT32_MAX)
			return false;
		if (parse::has_suffix(name, "Size")) {
			std::string_view texname = name;
			texname.remove_suffix(4);
			if (std::optional<KnownTexture> known = KnownTextureLookup.ToEnum(texname)) {
				BoundTexture tex = {};
				if (get_texture_info(&tex, *known, idx, pass_no)) {
					out->index   = tex.index;
					out->history = tex.history;
					switch (tex.source) {
						case TextureSource::User:
							if (tex.index >= chain.num_explicit_textures) {
								report_error("Texture out of bounds", name.data());
								return false;
							}
							out->type = BufferElementType::TextureSize;
							break;
						case TextureSource::Pass:
							if (tex.index >= chain.passes.size()) {
								report_error("Pass out of bounds", name.data());
								return false;
							}
							out->type = BufferElementType::PassOutputSize;
							break;
						case TextureSource::Input:
							out->type = BufferElementType::InputSize;
							break;
					}
					return true;
				}
			}
			// In addition to the normal texture ones, you can use `OutputSize` to get the size of the current pass's output
			if (texname == "Output") {
				out->type    = BufferElementType::PassOutputSize;
				out->index   = pass_no;
				out->history = 0;
				return true;
			}
		}
		if (idx)
			return false;
		std::optional<KnownParameter> param = KnownParameterLookup.ToEnum(name);
		if (!param)
			return false;
		out->type    = get_buffer_element_type(*param);
		out->index   = 0;
		out->history = 0;
		return true;
	}

	void handle_buffer(BoundBuffer* out, uint32_t id) {
		uint32_t pointer_id = decls[id].inst[1];
		if (decls[pointer_id].inst.op() != spv::OpTypePointer) {
			report_error("Buffer type wasn't pointer");
			return;
		}
		uint32_t struct_id = decls[pointer_id].inst[3];
		if (struct_id >= decls.size() || !decls[struct_id].inst || decls[struct_id].inst.op() != spv::OpTypeStruct) {
			report_error("Buffer pointee wasn't struct");
			return;
		}
		const SPIRVDeclaration& decl = decls[struct_id];
		StructInfo *info = find_struct(struct_id);
		if (!info) {
			report_error("Buffer missing struct info");
			return;
		}

		const std::vector<StructMemberInfo>& members = info->members;
		std::vector<BufferElement> elements;
		elements.reserve(members.size());
		uint32_t size = 0;

		for (size_t i = 0; i < members.size(); i++) {
			if (!members[i].name) {
				report_error("Buffer member missing name (required for reflection)", decl.name);
				return;
			}
			if (members[i].type == ShaderType::Unrecognized) {
				report_error("Buffer member had unrecognized type", members[i].name);
				continue;
			}
			if (members[i].offset == UINT32_MAX) {
				report_error("Buffer member missing offset", members[i].name);
			} else if (i > 0 && members[i - 1].offset != UINT32_MAX) {
				if (members[i - 1].offset > members[i].offset) {
					report_error("Buffer members out of order", members[i].name);
				} else if (members[i - 1].offset + get_size(members[i - 1].type) > members[i].offset) {
					report_error("Buffer member overlaps subsequent element", members[i - 1].name);
				}
			}
			size = std::max(size, members[i].offset + get_size(members[i].type));
			BufferElement element = {};
			element.offset = members[i].offset;
			if (!lookup_buffer_member_reference(&element, members[i].name)) {
				if (members[i].used)
					report_error("Unrecognized buffer member name", members[i].name);
				continue;
			}
			ShaderType shader_type = rashader::get_shader_type(element.type);
			if (shader_type != members[i].type) {
				report_error("Type of member doesn't match type expected from name", members[i].name);
				if (get_size(shader_type) != get_size(members[i].type))
					continue;
			}
			if (members[i].used)
				element.stages |= 1 << stage;
			elements.push_back(element);
		}

		out->stages |= 1 << stage;

		if (!out->contents.empty()) {
			if (out->binding != decls[id].binding)
				report_error("Buffer binding mismatch between shaders", decl.name);
			if (out->size != size)
				report_error("Buffer size mismatch between shaders", decl.name);
			if (out->contents.size() != elements.size()) {
				report_error("Buffer member count mismatch between shaders", decl.name);
				return;
			}
			for (size_t i = 0; i < elements.size(); i++) {
				if (out->contents[i] != elements[i]) {
					for (const StructMemberInfo& member : members) {
						if (member.offset == elements[i].offset) {
							report_error("Buffer member mismatch between shaders", member.name);
							break;
						}
					}
				} else {
					out->contents[i].stages |= elements[i].stages;
				}
			}
		} else {
			out->binding  = decls[id].binding;
			out->size     = size;
			out->contents = std::move(elements);
		}
	}

public:
	SPIRVParser(FilterChain& chain_, std::vector<SPIRVError>* errors_)
		: chain(chain_), errors(errors_)
	{
		for (size_t i = 0; i < chain.parameters.size(); i++)
			param_lookup.emplace(chain.parameters[i].name, static_cast<uint32_t>(i));
	}

	bool parse(uint32_t pass_no_, uint32_t stage_) {
		pass_no = pass_no_;
		stage = stage_;
		rashader::Pass& pass = chain.passes[pass_no];
		const std::vector<uint32_t>& spirv = stage ? pass.fs_spirv : pass.vs_spirv;
		if (spirv.size() < 6 || spirv[0] != spv::MagicNumber) {
			report_error("Invalid SPIRV module");
			return false;
		}

		const uint32_t *member_info_first = nullptr;
		const uint32_t *member_info_last  = nullptr;
		ubo  = UINT32_MAX;
		push = UINT32_MAX;
		decls.resize(spirv[3]);
		memset(decls.data(), 0, decls.size() * sizeof(decls[0]));
		structs.clear();
		uint32_t entry_id = UINT32_MAX;
		bool in_function = false;

		const uint32_t* end = spirv.data() + spirv.size();
		for (Instruction inst = Instruction(&spirv[5]); inst.words < end; inst = inst.next()) {
			spv::Op op = inst.op();
			if (inst.next().words > end) {
				report_error("SPIRV ends in middle of instruction");
				break;
			}
			SPIRVInstructionInfo info = get_instruction_info(op);
			if (!inst.size()) {
				report_error("Zero-length instruction");
				return false;
			}
			if (!info.min_size)
				continue;
			if (inst.size() < info.min_size) {
				report_error("Instruction too small");
				return false;
			}
			uint32_t target = info.min_size == 1 ? UINT32_MAX : inst[1 + info.has_result_type];
			if (info.min_size > 1 && target >= decls.size()) {
				report_error("Instruction targets OOB variable");
				return false;
			}
			if (info.has_result_type && inst[1] >= decls.size()) {
				report_error("Instruction has OOB type");
				return false;
			}
			if (info.is_definition) {
				decls[target].inst = inst;
			}
			if (op == spv::OpDecorate) {
				spv::Decoration decoration = static_cast<spv::Decoration>(inst[2]);
				switch (decoration) {
					case spv::DecorationBinding:
					case spv::DecorationDescriptorSet:
						if (inst.size() < 4) {
							report_error("Instruction too small");
							return false;
						}
						if (decoration == spv::DecorationDescriptorSet) {
							decls[target].has_set = 1;
							decls[target].set = inst[3];
						} else {
							decls[target].has_binding = 1;
							decls[target].binding = inst[3];
						}
						break;
					default:
						break;
				}
			}
			if (op == spv::OpName) {
				std::string_view view = inst.string(2);
				if (!view.empty() && view.data() + view.size() < reinterpret_cast<const char*>(inst.next().words))
					decls[inst[1]].name = view.data();
			}
			if (op == spv::OpTypeStruct) {
				auto it = std::lower_bound(structs.begin(), structs.end(), target, find_struct_compare);
				if (it != structs.end() && it->spirv_id == target) {
					report_error("Duplicate struct definition");
				} else {
					it = structs.emplace(it);
					it->spirv_id = target;
				}
			}
			if (op == spv::OpMemberName || op == spv::OpMemberDecorate) {
				if (!member_info_first)
					member_info_first = inst.words;
				member_info_last = inst.words;
			}
			if (op == spv::OpEntryPoint) {
				if (inst.string(3) == "main") {
					entry_id = inst[2];
				}
			}
			if (op == spv::OpFunction) {
				if (in_function) {
					report_error("Nested functions");
					return false;
				}
				in_function = true;
				decls[target].is_function = true;
			}
			if (op == spv::OpFunctionEnd) {
				if (!in_function) {
					report_error("Unmatched OpFunctionEnd");
					return false;
				}
				in_function = false;
			}
		}

		for (StructInfo& info : structs) {
			Instruction inst = decls[info.spirv_id].inst;
			info.members.resize(inst.size() - 2);
			for (uint32_t i = 2; i < inst.size(); i++)
				info.members[i - 2].type = get_shader_type(inst[i]);
		}

		if (member_info_first) {
			for (Instruction inst(member_info_first); inst.words <= member_info_last; inst = inst.next()) {
				spv::Op op = inst.op();
				if (op == spv::OpMemberName || op == spv::OpMemberDecorate) {
					if (StructInfo* info = find_struct(inst[1])) {
						if (inst[2] < info->members.size()) {
							if (op == spv::OpMemberName) {
								std::string_view view = inst.string(3);
								if (!view.empty() && view.data() + view.size() < reinterpret_cast<const char*>(inst.next().words))
									info->members[inst[2]].name = view.data();
							} else {
								switch (static_cast<spv::Decoration>(inst[3])) {
									case spv::DecorationOffset:
										if (inst.size() < 5) {
											report_error("Instruction too small");
											continue;
										}
										info->members[inst[2]].offset = inst[4];
										break;
									default:
										break;
								}
							}
						} else {
							report_error("Struct member op on out of bounds struct member");
						}
					} else {
						report_error("Struct member op on undeclared struct");
					}
				}
			}
		}

		if (in_function) {
			report_error("Unterminated function");
			return false;
		}

		if (entry_id >= decls.size() || !decls[entry_id].is_function) {
			report_error("Missing main function");
			return false;
		}

		if (!handle_function(entry_id))
			return false;

		for (const SPIRVDeclaration& decl : decls) {
			if (decl.has_binding) {
				if (decl.used) {
					if (decl.inst && decl.inst.op() == spv::OpVariable) {
						if (decl.inst[3] == spv::StorageClassUniformConstant) {
							handle_texture(decl);
						}
					} else {
						report_error("Non-variable op with set and binding", decl.name);
					}
				} else {
					// Don't warn, unused shader inputs are common as things are often shared between VS and FS even if one doesn't use them
				}
			}
		}

		if (ubo != UINT32_MAX) {
			handle_buffer(&chain.passes[pass_no].ubo, ubo);
		}

		if (push != UINT32_MAX) {
			handle_buffer(&chain.passes[pass_no].push, push);
		}

		return true;
	}
};

} // namespace rashader

bool rashader::parse_spirv(FilterChain& chain, std::vector<SPIRVError>* errors) {
	bool res = true;
	size_t count = chain.passes.size();
	SPIRVParser parser(chain, errors);
	for (size_t i = 0; i < count; i++) {
		res &= parser.parse(static_cast<uint32_t>(i), 0);
		res &= parser.parse(static_cast<uint32_t>(i), 1);
	}
	return res;
}

bool rashader::parse_spirv(FilterChain& chain, size_t pass_no, std::vector<SPIRVError>* errors) {
	SPIRVParser parser(chain, errors);
	bool vs = parser.parse(static_cast<uint32_t>(pass_no), 0);
	bool fs = parser.parse(static_cast<uint32_t>(pass_no), 1);
	return vs && fs;
}

const char* to_string(rashader::TextureSource source)   { return TextureSourceLookup.ToString(source); }
const char* to_string(rashader::BufferElementType type) { return BufferElementTypeLookup.ToString(type); }
