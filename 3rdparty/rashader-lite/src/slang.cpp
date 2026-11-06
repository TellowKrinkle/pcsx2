// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: MIT

#include <rashader-lite.h>
#include "common.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <list>

std::optional<std::string> rashader::default_file_read_callback(void* ctx, const char* path) {
	FILE* file = fopen(path, "rb");
	if (!file)
		return std::nullopt;
	std::string result;
	uint8_t buf[65536];
	size_t amt;
	while (0 != (amt = fread(buf, 1, sizeof(buf), file))) {
		result.append(buf, buf + amt);
	}
	int err = ferror(file);
	fclose(file);
	return err ? std::nullopt : std::make_optional(std::move(result));
}

namespace rashader {
namespace limits {
	// Some arbitrary limits to prevent us from running out of memory
	static constexpr uint32_t RECURSION = 16;
	static constexpr uint32_t PASSES = 65536;
}
namespace parse {

#ifdef _WIN32
static constexpr char PATH_SEPARATOR = '\\';
#else
static constexpr char PATH_SEPARATOR = '/';
#endif
static bool ispathseparator(unsigned char c) {
	if (c == '/')
		return true;
	if (c == '\\')
		return true;
	return false;
}

static const char* find(std::string_view str, int target) {
	return static_cast<const char*>(memchr(str.data(), target, str.size()));
}

static const char* find_last(std::string_view str, int target) {
	size_t n = str.find_last_of(target);
	return n == str.npos ? nullptr : &str[n];
}

static NOINLINE std::string_view trim_leading_whitespace(std::string_view line) {
	while (!line.empty() && iswhitespace(line.front()))
		line.remove_prefix(1);
	return line;
}

static NOINLINE std::string_view trim_whitespace(std::string_view line) {
	while (!line.empty() && iswhitespace(line.back()))
		line.remove_suffix(1);
	while (!line.empty() && iswhitespace(line.front()))
		line.remove_prefix(1);
	return line;
}

static std::string_view consume_n(std::string_view* line, size_t amt) {
	std::string_view result(line->data(), amt);
	line->remove_prefix(amt);
	return result;
}

static std::string_view partition(std::string_view* line, const char* pivot) {
	std::string_view result(line->data(), pivot - line->data());
	++pivot;
	*line = std::string_view(pivot, line->data() + line->size() - pivot);
	return result;
}

static std::optional<std::string_view> parse_string(std::string_view* line) {
	if (line->empty()) {
		return std::nullopt;
	} else if (line->front() == '"') {
		line->remove_prefix(1);
		size_t end;
		if (const char* end = find(*line, '"')) {
			return partition(line, end);
		}
	} else {
		for (size_t i = 0; i < line->size(); i++) {
			if (iswhitespace((*line)[i]))
				return consume_n(line, i);
		}
	}
	return consume_n(line, line->size());
}

static std::optional<float> parse_float(std::string_view* line) {
	char* end;
	const char* beg = line->data();
	float res = std::strtof(beg, &end);
	if (beg != end && end <= line->data() + line->size() && (iswhitespace(*end) || end == line->data() + line->size())) {
		*line = line->substr(end - line->data());
		return res;
	}
	return std::nullopt;
}

static std::optional<float> parse_float_full(std::string_view line) {
	if (line.empty())
		return std::nullopt;
	char* end;
	const char* beg = line.data();
	float res = std::strtof(beg, &end);
	if (end == line.data() + line.size())
		return res;
	return std::nullopt;
}

static std::optional<bool> parse_bool_full(std::string_view line) {
	if (line == "true" || line == "1")
		return true;
	if (line == "false" || line == "0")
		return false;
	return std::nullopt;
}

/// Parse a line like `something = "something"`
static std::optional<std::pair<std::string_view, std::string_view>> parse_slangp_line(std::string_view* line_) {
	std::string_view line = *line_;
	const char* equals = find(line, '=');
	if (!equals)
		return std::nullopt;
	std::string_view lhs = trim_whitespace(partition(&line, equals));
	line = trim_leading_whitespace(line);
	std::optional<std::string_view> rhs = parse_string(&line);
	if (!rhs)
		return std::nullopt;
	*line_ = line;
	return std::make_pair(lhs, *rhs);
}

} // namespace parse

struct LineIter {
	const char* data;
	std::string_view cur_line_;
	size_t next_begin = 0;
	size_t line_number_ = 0;
	LineIter(const std::string& str): data(str.c_str()) {
		next_line();
	}
public:
	void next_line() {
		size_t next_end = next_begin;
		while (!parse::isnewline(data[next_end]) && data[next_end] != '\0')
			++next_end;
		cur_line_ = std::string_view(data + next_begin, next_end - next_begin);
		if (data[next_end] == '\r')
			++next_end;
		if (data[next_end] == '\n')
			++next_end;
		next_begin = next_end;
		++line_number_;
	}
	bool has_line() const {
		return *cur_line_.data();
	}
	std::string_view cur_line() const { return cur_line_; }
	size_t line_number() const { return line_number_; }
};

enum class ShaderStage {
	Vertex = 1,
	Fragment = 2,
	All = Vertex | Fragment,
};

static bool operator&(ShaderStage a, ShaderStage b) { return static_cast<int>(a) & static_cast<int>(b); }

static std::string combine_path(const char* base_, std::string_view append) {
	std::string_view base(base_);
	while (!base.empty() && !parse::ispathseparator(base.back()))
		base.remove_suffix(1);
	while (!base.empty() && (parse::has_prefix(append, "../") || parse::has_prefix(append, "..\\"))) {
		append.remove_prefix(3);
		while (!append.empty() && parse::ispathseparator(append[0]))
			append.remove_prefix(1);
		base.remove_suffix(1);
		while (!base.empty() && !parse::ispathseparator(base.back()))
			base.remove_suffix(1);
	}
	std::string result;
	result.reserve(base.size() + append.size());
	result.append(base);
	for (char c : append)
		result.push_back(parse::ispathseparator(c) ? parse::PATH_SEPARATOR : c);
	return result;
}

static bool read_file(std::string* out, const char* path, std::vector<Error>* errors, FileReadCallback read, void *read_ctx, const char* parent_path, size_t parent_line) {
	std::optional<std::string> data = read(read_ctx, path);
	if (!data) {
		if (errors) errors->emplace_back(parent_path, parent_line, "Failed to read file", "");
		return false;
	}
	*out = *data;
	return true;
}

enum class Pragma { Stage, Name, Format, Parameter, IncludeOptional, Last = IncludeOptional };
enum class FileScaleType { Source, Viewport, Absolute, Last = Absolute };
enum class ShaderParam {
	Shader,
	FilterLinear,
	WrapMode,
	MipmapInput,
	Alias,
	FloatFramebuffer,
	SRGBFramebuffer,
	ScaleType,
	ScaleTypeX,
	ScaleTypeY,
	Scale,
	ScaleX,
	ScaleY,
	FrameCountMod,
	Last = FrameCountMod,
};
enum class TextureParam { Linear, WrapMode, Mipmap };

} // namespace rashader

static constexpr std::string_view FileScaleTypeName(rashader::FileScaleType type) {
	switch (type) {
		case rashader::FileScaleType::Source:   return "source";
		case rashader::FileScaleType::Viewport: return "viewport";
		case rashader::FileScaleType::Absolute: return "absolute";
	}
}

static constexpr std::string_view ScaleTypeName(rashader::ScaleType type) {
	switch (type) {
		case rashader::ScaleType::Input:    return "input";
		case rashader::ScaleType::Viewport: return "viewport";
		case rashader::ScaleType::Absolute: return "absolute";
	}
}

static constexpr std::string_view TextureFormatName(rashader::TextureFormat fmt) {
	switch (fmt) {
#define CASE(x) case rashader::TextureFormat::x: return #x
		CASE(R8_UNORM);
		CASE(R8_UINT);
		CASE(R8_SINT);
		CASE(R8G8_UNORM);
		CASE(R8G8_UINT);
		CASE(R8G8_SINT);
		CASE(R8G8B8A8_UNORM);
		CASE(R8G8B8A8_UINT);
		CASE(R8G8B8A8_SINT);
		CASE(R8G8B8A8_SRGB);
		CASE(A2B10G10R10_UNORM_PACK32);
		CASE(A2B10G10R10_UINT_PACK32);
		CASE(R16_UINT);
		CASE(R16_SINT);
		CASE(R16_SFLOAT);
		CASE(R16G16_UINT);
		CASE(R16G16_SINT);
		CASE(R16G16_SFLOAT);
		CASE(R16G16B16A16_UINT);
		CASE(R16G16B16A16_SINT);
		CASE(R16G16B16A16_SFLOAT);
		CASE(R32_UINT);
		CASE(R32_SINT);
		CASE(R32_SFLOAT);
		CASE(R32G32_UINT);
		CASE(R32G32_SINT);
		CASE(R32G32_SFLOAT);
		CASE(R32G32B32A32_UINT);
		CASE(R32G32B32A32_SINT);
		CASE(R32G32B32A32_SFLOAT);
#undef CASE
	}
}

static constexpr std::string_view WrapModeName(rashader::WrapMode mode) {
	switch (mode) {
		case rashader::WrapMode::ClampToEdge:    return "clamp_to_edge";
		case rashader::WrapMode::ClampToBorder:  return "clamp_to_border";
		case rashader::WrapMode::Repeat:         return "repeat";
		case rashader::WrapMode::MirroredRepeat: return "mirrored_repeat";
	}
}

static constexpr std::string_view PragmaName(rashader::Pragma pragma) {
	switch (pragma) {
		case rashader::Pragma::Stage:           return "stage";
		case rashader::Pragma::Name:            return "name";
		case rashader::Pragma::Format:          return "format";
		case rashader::Pragma::Parameter:       return "parameter";
		case rashader::Pragma::IncludeOptional: return "include_optional";
	}
}

static constexpr std::string_view ShaderParamName(rashader::ShaderParam directive) {
	switch (directive) {
		case rashader::ShaderParam::Shader:           return "shader";
		case rashader::ShaderParam::FilterLinear:     return "filter_linear";
		case rashader::ShaderParam::WrapMode:         return "wrap_mode";
		case rashader::ShaderParam::MipmapInput:      return "mipmap_input";
		case rashader::ShaderParam::Alias:            return "alias";
		case rashader::ShaderParam::FloatFramebuffer: return "float_framebuffer";
		case rashader::ShaderParam::SRGBFramebuffer:  return "srgb_framebuffer";
		case rashader::ShaderParam::ScaleType:        return "scale_type";
		case rashader::ShaderParam::ScaleTypeX:       return "scale_type_x";
		case rashader::ShaderParam::ScaleTypeY:       return "scale_type_y";
		case rashader::ShaderParam::Scale:            return "scale";
		case rashader::ShaderParam::ScaleX:           return "scale_x";
		case rashader::ShaderParam::ScaleY:           return "scale_y";
		case rashader::ShaderParam::FrameCountMod:    return "frame_count_mod";
	}
}

static constexpr auto FileScaleTypeLookup = rashader::make_enum_lookup<FileScaleTypeName>();
static constexpr auto ScaleTypeLookup     = rashader::make_enum_lookup<ScaleTypeName>();
static constexpr auto TextureFormatLookup = rashader::make_enum_lookup<TextureFormatName>();
static constexpr auto WrapModeLookup      = rashader::make_enum_lookup<WrapModeName>();
static constexpr auto PragmaLookup        = rashader::make_enum_lookup<PragmaName>();
static constexpr auto ShaderParamLookup   = rashader::make_enum_lookup<ShaderParamName>();

static size_t add_parameter(rashader::FilterChain* chain, std::string_view name, rashader::NameLookup& lookup) {
	auto it = lookup.find(name);
	if (it != lookup.end())
		return it->second;
	size_t idx = chain->parameters.size();
	chain->parameters.emplace_back();
	chain->parameters.back().name = name;
	lookup.emplace(chain->parameters.back().name, static_cast<uint32_t>(idx));
	return idx;
}

namespace rashader {

struct ParserBase {
	std::vector<Error>* errors;
	FileReadCallback read;
	void *read_ctx;

	uint32_t recursion_depth = 0;
	bool fatal_error = false;

	bool increment_recursion(const char* path, size_t line_no) {
		++recursion_depth;
		if (recursion_depth > limits::RECURSION) {
			if (errors) errors->emplace_back(path, line_no, "Include recursion limit exceeded", "");
			fatal_error = true;
			return false;
		}
		return true;
	}

	bool decrement_recursion() {
		--recursion_depth;
		return !fatal_error;
	}

	void NOINLINE report_error(const char* path, size_t line_no, const char* message, std::string_view extra = {}) {
		if (errors && (errors->empty() || errors->back().line != line_no || errors->back().file != path))
			errors->emplace_back(path, line_no, message, std::string(extra));
	}

	ParserBase(std::vector<Error>* errors_, FileReadCallback read_, void *read_ctx_)
		: errors(errors_), read(read_), read_ctx(read_ctx_) {}
};

class SlangParser: private ParserBase {
	const SlangParseConfig& config;
	FilterChain& dst_chain;
	Pass& dst_pass;
	NameLookup& param_lookup;
	ShaderStage stage = ShaderStage::All;

	template <typename Result>
	std::optional<Result> parse_argument(std::optional<Result> (*parse)(std::string_view*), std::string_view* line, const char* path, size_t line_no) {
		std::optional<Result> res = parse(line);
		if (!res)
			report_error(path, line_no, std::is_same_v<Result, float> && !line->empty() ? "Invalid float argument" : "Missing argument");
		*line = parse::trim_leading_whitespace(*line);
		return res;
	}

	bool handle_include(std::string_view& line, const char* path, size_t line_no, bool should_report_error) {
		if (!increment_recursion(path, line_no))
			return false;
		if (auto arg = parse_argument(parse::parse_string, &line, path, line_no)) {
			std::string include = combine_path(path, *arg);
			if (auto data = read(read_ctx, include.c_str()))
				run(include.c_str(), *data);
			else if (should_report_error)
				report_error(path, line_no, "Failed to read file", include);
		}
		if (!decrement_recursion())
			return false;
		return true;
	}

	void append_line(std::string_view line) {
		if (stage & ShaderStage::Vertex) {
			dst_pass.vs_glsl.append(line);
			dst_pass.vs_glsl.push_back('\n');
		}
		if (stage & ShaderStage::Fragment) {
			dst_pass.fs_glsl.append(line);
			dst_pass.fs_glsl.push_back('\n');
		}
	}

public:
	SlangParser(FilterChain& dst_chain_, Pass& dst_pass_, NameLookup& param_lookup_, std::vector<Error>* errors_, const SlangParseConfig& config_, FileReadCallback read_, void *read_ctx_)
		: ParserBase(errors_, read_, read_ctx_), config(config_), dst_chain(dst_chain_), dst_pass(dst_pass_), param_lookup(param_lookup_) {}

	void run(const char* path, const std::string& data) {
		for (LineIter iter(data); iter.has_line(); iter.next_line()) {
			std::string_view line = parse::trim_leading_whitespace(iter.cur_line());
			if (parse::try_consume_prefix(&line, "#version")) {
				append_line(iter.cur_line());
				// Also add feature defines
				if (config.has_original_aspect)
					append_line("#define _HAS_ORIGINALASPECT_UNIFORMS");
				if (config.has_frametime)
					append_line("#define _HAS_FRAMETIME_UNIFORMS");
				if (config.has_rotation)
					append_line("#define _HAS_ROTATION_UNIFORM");
				if (config.has_sensors)
					append_line("#define _HAS_SENSOR_UNIFORMS");
			} else if (parse::try_consume_prefix(&line, "#pragma")) {
				line = parse::trim_leading_whitespace(line);
				if (std::optional<std::string_view> pragma_ = parse::parse_string(&line)) {
					line = parse::trim_leading_whitespace(line);
					std::optional<Pragma> pragma = PragmaLookup.ToEnum(*pragma_);
					if (*pragma_ == "alias")
						pragma = Pragma::Name;
					if (pragma) {
						switch (*pragma) {
							case Pragma::Stage:
								if (auto arg = parse_argument(parse::parse_string, &line, path, iter.line_number())) {
									if (*arg == "vertex")
										stage = ShaderStage::Vertex;
									else if (*arg == "fragment")
										stage = ShaderStage::Fragment;
									else
										report_error(path, iter.line_number(), "Unrecognized stage", *arg);
								}
								break;
							case Pragma::Name:
								if (!dst_pass.name.empty())
									report_error(path, iter.line_number(), "Multiple `#pragma name`s");
								if (auto arg = parse_argument(parse::parse_string, &line, path, iter.line_number()))
									dst_pass.name = *arg;
								break;
							case Pragma::Format:
								if (auto arg = parse_argument(parse::parse_string, &line, path, iter.line_number())) {
									if (auto format = TextureFormatLookup.ToEnum(*arg)) {
										dst_pass.out_format = *format;
									} else {
										report_error(path, iter.line_number(), "Unrecognized format", *arg);
									}
								}
								break;
							case Pragma::Parameter: {
								auto arg0 = parse_argument(parse::parse_string, &line, path, iter.line_number());
								auto arg1 = parse_argument(parse::parse_string, &line, path, iter.line_number());
								auto arg2 = parse_argument(parse::parse_float, &line, path, iter.line_number());
								auto arg3 = parse_argument(parse::parse_float, &line, path, iter.line_number());
								auto arg4 = parse_argument(parse::parse_float, &line, path, iter.line_number());
								auto arg5 = parse::parse_float(&line).value_or(0);
								if (arg0 && arg1 && arg2 && arg3 && arg4) {
									size_t idx = add_parameter(&dst_chain, *arg0, param_lookup);
									Parameter& param = dst_chain.parameters[idx];
									dst_pass.parameters.push_back(static_cast<uint32_t>(idx));
									if (param.declared) {
										if (param.description != *arg1 || param.initial != *arg2 || param.minimum != *arg3 || param.maximum != *arg4 || param.step != arg5) {
											report_error(path, iter.line_number(), "Parameter definition doesn't match previous");
										}
									}
									param.declared = true;
									param.description = *arg1;
									param.initial = *arg2;
									param.minimum = *arg3;
									param.maximum = *arg4;
									param.step = arg5;
									param.value = param.initial;
								} else {
									report_error(path, iter.line_number(), "Failed to parse parameter arguments", arg0 ? *arg0 : std::string_view{});
								}
								break;
							}
							case Pragma::IncludeOptional:
								if (!handle_include(line, path, iter.line_number(), false))
									return;
								break;
						}
						line = parse::trim_leading_whitespace(line);
						if (!line.empty() && !parse::has_prefix(line, "//")) {
							report_error(path, iter.line_number(), "Too many arguments");
						}
					} else {
						append_line(iter.cur_line());
						report_error(path, iter.line_number(), "Unrecognized pragma", *pragma_);
					}
				} else {
					report_error(path, iter.line_number(), "Empty pragma");
				}
			} else if (parse::try_consume_prefix(&line, "#include")) {
				line = parse::trim_leading_whitespace(line);
				if (!handle_include(line, path, iter.line_number(), true))
					return;
			} else {
				append_line(iter.cur_line());
			}
		}
	}
};

/// Shader info required for parsing a slangp but no longer needed after parsing completes
struct ShaderInfo {
	std::string alias;
	const char* decl_path;
	size_t decl_line;
	const char* alias_path;
	size_t alias_line;
	bool float_framebuffer = false;
	bool srgb_framebuffer = false;
	uint32_t shader_info_seen = 0;
	const char* framebuffer_path = nullptr;
	size_t framebuffer_line = 0;
	FileScaleType scale_type_x = FileScaleType::Source;
	FileScaleType scale_type_y = FileScaleType::Source;
};

/// Texture info required for parsing a slangp but no longer needed after parsing completes
struct TextureInfo {
	const char* decl_path;
	size_t decl_line;
	uint32_t params_seen = 0;
};

class SlangPParser: private ParserBase {
	const SlangParseConfig& config;
	FilterChain& res;
	std::vector<ShaderInfo> shader_info;
	std::vector<TextureInfo> texture_info;
	std::vector<std::pair<std::string, float>> set_parameters;
	std::vector<std::string> explicit_parameters;
	std::list<std::string> paths;
	NameLookup param_lookup;
	uint64_t declared_shader_count = 0;
	bool has_explicit_parameters = false;

	void handle_texture_list(std::string_view list, const char* path, size_t line_no) {
		bool changed = false;
		uint32_t count = 0;
		while (!list.empty()) {
			const char* semi = parse::find(list, ';');
			std::string_view name;
			if (semi) {
				name = parse::partition(&list, semi);
			} else {
				name = list;
				list = {};
			}
			uint32_t idx;
			for (idx = 0; idx < res.textures.size(); idx++) {
				if (res.textures[idx].name == name)
					break;
			}
			if (idx == res.textures.size()) {
				res.textures.emplace(res.textures.begin() + count)->name = name;
				auto info = texture_info.emplace(texture_info.begin() + count);
				info->decl_path = path;
				info->decl_line = line_no;
			} else if (idx > count) {
				std::rotate(res.textures.begin() + count, res.textures.begin() + idx, res.textures.begin() + idx + 1);
				std::rotate(texture_info.begin() + count, texture_info.begin() + idx, texture_info.begin() + idx + 1);
				changed = true;
			} else if (idx < count) {
				report_error(path, line_no, "Duplicate texture in list");
				continue;
			}
			++count;
		}
		if (res.num_explicit_textures != UINT32_MAX) {
			if (changed || res.num_explicit_textures != count)
				report_error(path, line_no, "Duplicate texture lists don't match");
		} else {
			res.num_explicit_textures = count;
		}
	}

	void handle_parameter_list(std::string_view list, const char* path, size_t line_no) {
		uint32_t count = 0;
		for (; !list.empty(); ++count) {
			const char* semi = parse::find(list, ';');
			std::string_view name;
			if (semi) {
				name = parse::partition(&list, semi);
			} else {
				name = list;
				list = {};
			}
			if (has_explicit_parameters) {
				if (explicit_parameters.size() <= count || explicit_parameters[count] != name) {
					report_error(path, line_no, "Duplicate parameter lists don't match");
					return;
				}
			} else {
				explicit_parameters.emplace_back(name);
			}
		}
		if (has_explicit_parameters) {
			if (explicit_parameters.size() != count)
				report_error(path, line_no, "Duplicate parameter lists don't match");
		} else {
			has_explicit_parameters = true;
		}
	}

	std::string_view skip_comments(LineIter& iter, std::string_view line) {
		if (parse::has_prefix(line, "//") || parse::has_prefix(line, "#"))
			return {};
		if (parse::try_consume_prefix(&line, "/*")) {
			while (true) {
				if (const char* end = parse::find(line, '*')) {
					parse::partition(&line, end);
					if (line.data()[0] == '/') {
						line.remove_prefix(1);
						break;
					}
					continue;
				}
				iter.next_line();
				if (!iter.has_line())
					break;
				line = iter.cur_line();
			}
			return parse::trim_leading_whitespace(line);
		}
		return line;
	}

	bool parse_pass_info(FilterChain& res, std::string_view key, std::string_view val, const char* path, uint64_t line_no) {
		auto idx_ = parse::parse_uint64_suffix(&key);
		if (!idx_)
			return false;
		uint64_t idx = *idx_;
		auto param = ShaderParamLookup.ToEnum(key);
		if (key == "texture_wrap_mode") // Special handling for CRT Royale
			param = ShaderParam::WrapMode;
		if (key == "framecount_mod") // Special handling for Border IQ
			param = ShaderParam::FrameCountMod;
		if (!param) {
			return false;
		}
		if (idx >= res.passes.size()) {
			if (idx >= limits::PASSES) {
				report_error(path, line_no, "Shader index too large");
				return true;
			}
			size_t prev_end = res.passes.size();
			shader_info.resize(idx + 1);
			res.passes.resize(idx + 1);
			for (; prev_end <= idx; prev_end++) {
				shader_info[prev_end].decl_path = path;
				shader_info[prev_end].decl_line = line_no;
			}
		}

		uint32_t mask = 1 << static_cast<uint32_t>(*param);
		if (*param == ShaderParam::ScaleType)
			mask |= (1 << static_cast<uint32_t>(ShaderParam::ScaleTypeX)) | (1 << static_cast<uint32_t>(ShaderParam::ScaleTypeY));
		if (*param == ShaderParam::Scale)
			mask |= (1 << static_cast<uint32_t>(ShaderParam::ScaleX)) | (1 << static_cast<uint32_t>(ShaderParam::ScaleY));
		if (shader_info[idx].shader_info_seen & mask)
			report_error(path, line_no, "Duplicate shader parameter");

		switch (*param) {
			case ShaderParam::Shader:
				res.passes[idx].path = combine_path(path, val);
				shader_info[idx].decl_path = path;
				shader_info[idx].decl_line = line_no;
				break;
			case ShaderParam::WrapMode:
				if (auto arg = WrapModeLookup.ToEnum(val)) {
					res.passes[idx].wrap = *arg;
				} else {
					report_error(path, line_no, "Invalid wrap mode", val);
				}
				break;

			case ShaderParam::FloatFramebuffer:
			case ShaderParam::SRGBFramebuffer:
				shader_info[idx].framebuffer_path = path;
				shader_info[idx].framebuffer_line = line_no;
				[[fallthrough]];
			case ShaderParam::FilterLinear:
			case ShaderParam::MipmapInput:
				if (auto arg = parse::parse_bool_full(val)) {
					bool* target = nullptr;
					switch (*param) {
						case ShaderParam::FilterLinear:     target = &res.passes[idx].filter_linear;      break;
						case ShaderParam::MipmapInput:      target = &res.passes[idx].mipmap_input;       break;
						case ShaderParam::FloatFramebuffer: target = &shader_info[idx].float_framebuffer; break;
						case ShaderParam::SRGBFramebuffer:  target = &shader_info[idx].srgb_framebuffer;  break;
						default: break;
					}
					*target = *arg;
				} else {
					report_error(path, line_no, "Invalid bool", val);
				}
				break;
			case ShaderParam::Alias:
				if (!shader_info[idx].alias.empty())
					report_error(path, line_no, "Only one alias directive is allowed per shader");
				shader_info[idx].alias = val;
				shader_info[idx].alias_path = path;
				shader_info[idx].alias_line = line_no;
				break;
			case ShaderParam::ScaleType:
			case ShaderParam::ScaleTypeX:
			case ShaderParam::ScaleTypeY:
				if (auto arg = FileScaleTypeLookup.ToEnum(val)) {
					if (*param == ShaderParam::ScaleType || *param == ShaderParam::ScaleTypeX)
						shader_info[idx].scale_type_x = *arg;
					if (*param == ShaderParam::ScaleType || *param == ShaderParam::ScaleTypeY)
						shader_info[idx].scale_type_y = *arg;
				} else {
					report_error(path, line_no, "Invalid scale type", val);
				}
				break;
			case ShaderParam::Scale:
			case ShaderParam::ScaleX:
			case ShaderParam::ScaleY:
				if (auto arg = parse::parse_float_full(val)) {
					if (*param == ShaderParam::Scale || *param == ShaderParam::ScaleX)
						res.passes[idx].scale_x = *arg;
					if (*param == ShaderParam::Scale || *param == ShaderParam::ScaleY)
						res.passes[idx].scale_y = *arg;
				} else {
					report_error(path, line_no, "Invalid float", val);
				}
				break;
			case ShaderParam::FrameCountMod:
				if (auto arg = parse::parse_uint64_full(val)) {
					if (*arg <= UINT32_MAX)
						res.passes[idx].frame_count_mod = static_cast<uint32_t>(*arg);
					else
						report_error(path, line_no, "Frame count mod too large", val);
				} else {
					report_error(path, line_no, "Invalid uint", val);
				}
				break;
		}
		return true;
	}

	static bool has_file_extension(std::string_view str) {
		const char* ext = parse::find_last(str, '.');
		if (!ext)
			return false;
		++ext;
		const char* end = str.data() + str.size();
		bool non_number = false;
		for (; ext < end; ++ext) {
			char c = *ext;
			if (parse::isalpha(c)) {
				non_number = true;
				continue;
			}
			if (parse::isdigit(c))
				continue;
			return false;
		}
		// Needs at least one non-numeric character after the decimal
		return non_number;
	}

	std::optional<size_t> find_texture(std::string_view name) const {
		for (size_t i = 0; i < res.textures.size(); i++)
			if (res.textures[i].name == name)
				return i;
		return std::nullopt;
	}

	size_t add_texture(std::string_view name) {
		if (std::optional<size_t> tex = find_texture(name))
			return *tex;
		assert(res.textures.size() == texture_info.size());
		size_t idx = res.textures.size();
		texture_info.emplace_back();
		res.textures.emplace_back();
		res.textures.back().name = name;
		return idx;
	}

	std::string_view parse_line(FilterChain& res, std::string_view line, const char* path, uint64_t line_no) {
		if (auto data = parse::parse_slangp_line(&line)) {
			line = parse::trim_leading_whitespace(line);
			// shaders = <number>
			if (data->first == "shaders") {
				if (declared_shader_count == 0) {
					if (auto val = parse::parse_uint64_full(data->second)) {
						uint64_t npasses = *val;
						if (npasses > limits::PASSES) {
							report_error(path, line_no, "Too many shader passes");
							npasses = limits::PASSES;
						}
						if (npasses > res.passes.size()) {
							res.passes.resize(npasses);
							shader_info.resize(npasses);
						}
						declared_shader_count = npasses;
					} else {
						report_error(path, line_no, "Invalid shader count");
					}
				} else {
					report_error(path, line_no, "Only one shaders directive allowed");
				}
				return line;
			}

			// textures = "a;b;c;"
			if (data->first == "textures") {
				handle_texture_list(data->second, path, line_no);
				return line;
			}

			// parameters = "a;b;c"
			if (data->first == "parameters") {
				handle_parameter_list(data->second, path, line_no);
				return line;
			}

			// Try to parse pass info
			if (parse_pass_info(res, data->first, data->second, path, line_no))
				return line;

			// Try to parse texture info
			for (size_t i = 0; i < res.textures.size(); i++) {
				Texture& tex = res.textures[i];
				std::string_view name = data->first;
				TextureParam param;
				if (parse::try_consume_prefix(&name, tex.name)) {
					if (name.empty()) {
						// Don't warn on duplicate.  This is commonly done with a placeholder set in one file, then overridden in another.
						tex.path = combine_path(path, data->second);
						texture_info[i].decl_path = path;
						texture_info[i].decl_line = line_no;
						return line;
					} else if (name == "_linear" || name == "_filter_linear") {
						param = TextureParam::Linear;
					} else if (name == "_wrap_mode" || name == "_repeat_mode") {
						param = TextureParam::WrapMode;
					} else if (name == "_mipmap") {
						param = TextureParam::Mipmap;
					} else {
						continue;
					}
				} else {
					continue;
				}
				uint32_t mask = 1 << static_cast<uint32_t>(param);
				if (texture_info[i].params_seen & mask)
					report_error(path, line_no, "Duplicate texture parameter");
				texture_info[i].params_seen |= mask;
				switch (param) {
					case TextureParam::WrapMode:
						if (auto val = WrapModeLookup.ToEnum(data->second))
							tex.wrap = *val;
						else
							report_error(path, line_no, "Invalid wrap mode", data->second);
						break;
					case TextureParam::Linear:
					case TextureParam::Mipmap:
						if (auto val = parse::parse_bool_full(data->second))
							(param == TextureParam::Linear ? tex.linear : tex.mipmap) = *val;
						else
							report_error(path, line_no, "Invalid boolean", data->second);
						break;
				}
				return line;
			}

			// If nothing matches, try to parse as parameter
			if (auto val = parse::parse_float_full(data->second)) {
				// Don't warn on duplicate.  This is commonly done with a placeholder set in one file, and then overridden in another.
				set_parameters.emplace_back(data->first, *val);
			} else if (has_file_extension(data->second)) {
				// If that fails, see if it looks like a texture
				size_t idx = add_texture(data->first);
				res.textures[idx].path = combine_path(path, data->second);
				texture_info[idx].decl_path = path;
				texture_info[idx].decl_line = line_no;
			} else {
				report_error(path, line_no, "Invalid float");
			}
		}
		return line;
	}

	void parse_file(const char* path, const std::string& data) {
		for (LineIter iter(data); iter.has_line(); iter.next_line()) {
			std::string_view line = parse::trim_leading_whitespace(iter.cur_line());
			// Handle #reference
			if (parse::try_consume_prefix(&line, "#reference")) {
				line = parse::trim_leading_whitespace(line);
				if (!increment_recursion(path, iter.line_number()))
					return;
				if (auto arg = parse::parse_string(&line)) {
					std::string& reference_path = paths.emplace_back();
					reference_path = combine_path(path, *arg);
					std::string reference;
					if (read_file(&reference, reference_path.c_str(), errors, read, read_ctx, path, iter.line_number()))
						parse_file(reference_path.c_str(), reference);
				} else {
					report_error(path, iter.line_number(), "Missing argument");
				}
				if (!decrement_recursion())
					return;
				line = parse::trim_leading_whitespace(line);
				if (!line.empty())
					report_error(path, iter.line_number(), "Failed to parse line");
				continue;
			}

			line = skip_comments(iter, line);

			if (line.empty())
				continue;

			const char* begin = line.data();
			line = parse_line(res, line, path, iter.line_number());
			line = skip_comments(iter, line);
			if (!line.empty())
				report_error(path, iter.line_number(), line.data() == begin ? "Failed to parse line" : "Unrecognized characters at end of line", line);
		}
	}

	void resolve_scale(FileScaleType type, ScaleType Pass::*member_type, float Pass::*member_scale, size_t idx) {
		ScaleType* target_type = &(res.passes[idx].*member_type);
		switch (type) {
			case FileScaleType::Viewport: *target_type = ScaleType::Viewport; break;
			case FileScaleType::Absolute: *target_type = ScaleType::Absolute; break;
			case FileScaleType::Source:
				if (idx == 0) {
					*target_type = ScaleType::Input;
				} else {
					*target_type = res.passes[idx - 1].*member_type;
					res.passes[idx].*member_scale *= res.passes[idx - 1].*member_scale;
				}
				break;
		}
	}

public:
	SlangPParser(FilterChain& res_, std::vector<Error>* errors_, const SlangParseConfig& config_, FileReadCallback read_, void *read_ctx_)
		: ParserBase(errors_, read_, read_ctx_), config(config_), res(res_) {}

	void run(const char* path, const std::string& data) {
		res.num_explicit_textures = UINT32_MAX;
		parse_file(path, data);
		if (res.num_explicit_textures == UINT32_MAX) res.num_explicit_textures = 0;
		for (size_t i = declared_shader_count; i < shader_info.size(); i++) {
			if (res.passes[i].path.empty()) {
				report_error(shader_info[i].decl_path, shader_info[i].decl_line, "Non-path parameters specified for shader but `shader =` parameter missing");
				shader_info.resize(i);
				res.passes.resize(i);
				break;
			}
		}
		for (size_t i = 0; i < shader_info.size(); i++) {
			if (res.passes[i].path.empty()) {
				report_error(path, i, "Shader path never specified");
			} else {
				SlangParser parser(res, res.passes[i], param_lookup, errors, config, read, read_ctx);
				const char* shader_path = res.passes[i].path.c_str();
				std::string data;
				if (read_file(&data, shader_path, errors, read, read_ctx, shader_info[i].decl_path, shader_info[i].decl_line))
					parser.run(shader_path, data);
				// srgb and float framebuffer slangp options override whatever the shader wanted (even if it was an explicit #pragma format)
				if (shader_info[i].srgb_framebuffer) {
					if (shader_info[i].float_framebuffer)
						report_error(shader_info[i].framebuffer_path, shader_info[i].framebuffer_line, "Can't use both float and srgb framebuffer");
					res.passes[i].out_format = TextureFormat::R8G8B8A8_SRGB;
				} else if (shader_info[i].float_framebuffer) {
					res.passes[i].out_format = TextureFormat::R16G16B16A16_SFLOAT;
				}
				if (!res.passes[i].name.empty() && !shader_info[i].alias.empty() && res.passes[i].name != shader_info[i].alias) {
					report_error(shader_info[i].alias_path, shader_info[i].alias_line, "Shader alias and name don't match",
					             res.passes[i].name + " != " + shader_info[i].alias);
				}
				if (!shader_info[i].alias.empty())
					res.passes[i].name = shader_info[i].alias;
			}
			resolve_scale(shader_info[i].scale_type_x, &Pass::scale_type_x, &Pass::scale_x, i);
			resolve_scale(shader_info[i].scale_type_y, &Pass::scale_type_y, &Pass::scale_y, i);
		}

		res.num_declared_parameters = static_cast<uint32_t>(res.parameters.size());
		res.explicit_parameters.reserve(explicit_parameters.size());
		for (const std::string& param : explicit_parameters) {
			size_t idx = add_parameter(&res, param, param_lookup);
			res.explicit_parameters.push_back(static_cast<uint32_t>(idx));
		}
		for (const auto& pair : set_parameters) {
			size_t idx = add_parameter(&res, pair.first, param_lookup);
			Parameter& param = res.parameters[idx];
			param.set = true;
			param.value = pair.second;
		}
		if (res.textures.size() != texture_info.size())
			abort();
		for (size_t i = 0; i < res.textures.size(); i++) {
			std::string_view name = res.textures[i].name;
			if (parse::try_consume_suffix(&name, "Feedback")) {
				if (find_texture(name))
					report_error(texture_info[i].decl_path, texture_info[i].decl_line, "Texture makes UBO value ambiguous", res.textures[i].name + "Size");
			}
		}
	}
};
} // namespace rashader

bool rashader::FilterChain::ok() const {
	// Must have at least one pass, and all passes must have shader code
	// (Any further judge on OK-ness will need to be left to the actual glsl compiler)
	if (passes.empty())
		return false;
	for (const Pass& pass : passes)
		if (pass.vs_glsl.empty() || pass.fs_glsl.empty())
			return false;
	return true;
}

rashader::FilterChain rashader::parse_slangp(const char* path, std::vector<Error>* errors, const SlangParseConfig& config, FileReadCallback read, void *read_ctx) {
	rashader::FilterChain res;
	SlangPParser parser(res, errors, config, read, read_ctx);
	std::string data;
	if (!read_file(&data, path, errors, read, read_ctx, path, 0))
		return res;
	parser.run(path, data);
	for (size_t i = 1; i < res.passes.size(); i++)
		res.passes[i - 1].mipmap_output = res.passes[i].mipmap_input;
	return res;
}

rashader::FilterChain rashader::parse_slang(const char* path, std::vector<Error>* errors, const SlangParseConfig& config, FileReadCallback read, void *read_ctx) {
	rashader::FilterChain res;
	res.passes.emplace_back();
	parse_slang(res, res.passes.back(), path, errors, config, read, read_ctx);
	return res;
}

void rashader::parse_slang(FilterChain& dst_chain, Pass& dst_pass, const char* path, std::vector<Error>* errors, const SlangParseConfig& config, FileReadCallback read, void *read_ctx) {
	NameLookup param_lookup;
	for (size_t i = 0; i < dst_chain.parameters.size(); i++)
		param_lookup.emplace(dst_chain.parameters[i].name, static_cast<uint32_t>(i));
	SlangParser parser(dst_chain, dst_pass, param_lookup, errors, config, read, read_ctx);
	std::string data;
	if (read_file(&data, path, errors, read, read_ctx, path, 0))
		parser.run(path, data);
	dst_pass.path = path;
	dst_chain.num_declared_parameters = static_cast<uint32_t>(dst_chain.parameters.size());
}

const char* to_string(rashader::ScaleType     val) { return ScaleTypeLookup.ToString(val); }
const char* to_string(rashader::TextureFormat val) { return TextureFormatLookup.ToString(val); }
const char* to_string(rashader::WrapMode      val) { return WrapModeLookup.ToString(val); }
