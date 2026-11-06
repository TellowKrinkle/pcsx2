// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <vector>
#include <cstdint>

namespace rashader {

typedef std::optional<std::string>(*FileReadCallback)(void *ctx, const char* path);
std::optional<std::string> default_file_read_callback(void *ctx, const char* path);

struct Error {
	/// The path to the file where this error occurred
	std::string file;
	/// The line in the file where this occurred
	size_t line;
	/// A generic message about the type of error that occurred (static lifetime)
	const char* message;
	/// An optional extra piece of information about e.g. what string was unrecognized
	std::string extra;
	Error(const char* file_, size_t line_, const char* message_, std::string extra_): file(file_), line(line_), message(message_), extra(std::move(extra_)) {}
};

struct SPIRVError {
	/// The pass index in the filter chain
	uint32_t pass;
	/// 0 for VS, 1 for FS
	uint32_t stage;
	/// A generic message about the type of error that occurred (static lifetime)
	const char* message;
	/// An optional string with the variable name the error occurred on (points into the spirv bytecode vector)
	const char* target;
	SPIRVError(uint32_t pass_, uint32_t stage_, const char* message_, const char* target_): pass(pass_), stage(stage_), message(message_), target(target_) {}
};

struct SlangParseConfig {
	bool has_original_aspect = true; ///< Whether to report to the shader that original aspect is supported
	bool has_frametime       = true; ///< Whether to report to the shader that frametime is supported
	bool has_rotation        = true; ///< Whether to report to the shader that rotation is supported
	bool has_sensors         = true; ///< Whether to report to the shader that accel/gyro is supported
};

enum class ScaleType : uint8_t {
	// `source` scaling in slangp files is automatically resolved to one of the other types during parsing.
	Input,    ///< Scaled relative to the input of the filter graph
	Viewport, ///< Scaled relative to the host display output
	Absolute, ///< Fixed size, not relative to anything
	Last = Absolute,
};
enum class TextureFormat : uint8_t {
	R8_UNORM,
	R8_UINT,
	R8_SINT,
	R8G8_UNORM,
	R8G8_UINT,
	R8G8_SINT,
	R8G8B8A8_UNORM,
	R8G8B8A8_UINT,
	R8G8B8A8_SINT,
	R8G8B8A8_SRGB,
	A2B10G10R10_UNORM_PACK32,
	A2B10G10R10_UINT_PACK32,
	R16_UINT,
	R16_SINT,
	R16_SFLOAT,
	R16G16_UINT,
	R16G16_SINT,
	R16G16_SFLOAT,
	R16G16B16A16_UINT,
	R16G16B16A16_SINT,
	R16G16B16A16_SFLOAT,
	R32_UINT,
	R32_SINT,
	R32_SFLOAT,
	R32G32_UINT,
	R32G32_SINT,
	R32G32_SFLOAT,
	R32G32B32A32_UINT,
	R32G32B32A32_SINT,
	R32G32B32A32_SFLOAT,
	Last = R32G32B32A32_SFLOAT,
};
enum class WrapMode : uint8_t { ClampToEdge, ClampToBorder, Repeat, MirroredRepeat, Last = MirroredRepeat };
enum class TextureSource : uint8_t {
	User,  ///< One of the textures supplied to the filter chain (`index` is index into the filter chain textures array)
	Pass,  ///< The output of a previous pass (`index` is the index into the filter chain pass array, `history` is how many frames old the result should be)
	Input, ///< The input to the rashader filter chain (`history` is how many frames old the result should be)
	Last = Input,
};
enum class BufferElementType : uint8_t {
	MVP,                    ///< (mat4)  Transformation matrix
	FinalViewportSize,      ///< (vec4)  Size of the filter chain's output (xy=wh, zw=1/wh)
	FrameCount,             ///< (uint)  Frame count, mod pass.frame_count_mod if nonzero
	Rotation,               ///< (uint)  0=0°, 1=90°, 2=180°, 3=270°
	OriginalAspect,         ///< (float) Aspect ratio of the frame (may not be same as w/h for emulated devices with non-square pixels)
	OriginalAspectRotated,  ///< (float) Aspect ratio after applying `Rotation`
	OriginalFPS,            ///< (float) Current frame rate of emulated console
	FrameTimeDelta,         ///< (uint)  Time difference between previous and current frame, in microseconds
	FrameDirection,         ///< (int)   Negative to indicate rewind, positive for frame advance.
	CurrentSubFrame,        ///< (uint)  Current subframe, in range `[1, TotalSubFrames]`
	TotalSubFrames,         ///< (uint)  Total number of subframes per frame
	HDRMode,                ///< (uint)  0=Off/SDR, 1=HDR10, 2=scRGB, 3=PQ->scRGB
	BrightnessNits,         ///< (float) core paper-white in nits
	ExpandGamut,            ///< (uint)  0=Accurate, 1=Expanded, 2=Wide, 3=Super
	Gyroscope,              ///< (vec3)  Gyroscope reading
	Accelerometer,          ///< (vec3)  Accelerometer reading
	AccelerometerRest,      ///< (vec3)  Accelerometer reading when device is at rest
	UserParameter,          ///< (float) User/slangp-supplied parameter at `index` in filter chain parameters array
	InputSize,              ///< (vec4)  Size of input to the rashader filter chain from `history` frames ago (xy=wh, zw=1/wh)
	PassOutputSize,         ///< (vec4)  Size of pass `index` output from `history` frames ago (xy=wh, zw=1/wh)
	TextureSize,            ///< (vec4)  Size of texture at index `index` in filter chain textures array (xy=wh, zw=1/wh)
	Last = TextureSize,
};
static constexpr uint32_t STAGE_FLAG_VS = 1;
static constexpr uint32_t STAGE_FLAG_FS = 2;

struct BufferElement {
	/// Offset in the UBO / Push descriptors
	uint32_t offset;
	/// Type / source of data
	BufferElementType type;
	/// 1: VS, 2: FS
	uint8_t stages;
	/// Index into source array (if applicable)
	uint32_t index;
	/// How many frames old the texture to source data from is (if applicable)
	uint32_t history;
};

static bool operator==(const BufferElement& a, const BufferElement& b) {
	return a.offset == b.offset && a.type == b.type && a.index == b.index && a.history == b.history;
}
static bool operator!=(const BufferElement& a, const BufferElement& b) { return !(a == b); }

struct BoundBuffer {
	/// Binding ID if UBO (ignored for push constants)
	uint32_t binding;
	/// The size of the buffer in bytes
	uint32_t size;
	/// 1: VS, 2: FS
	uint32_t stages;
	/// The data contained by the buffer
	std::vector<BufferElement> contents;
};

struct BoundTexture {
	/// Binding ID
	uint32_t binding;
	/// 1: VS, 2: FS
	uint8_t stages;
	/// Where the texture is coming from
	TextureSource source;
	/// Resolved value for wrap mode to use when sampling the texture
	WrapMode wrap;
	/// Resolved value for whether to use linear filtering when sampling the texture
	bool linear;
	/// The index into the source array (if applicable)
	uint32_t index;
	/// How many frames old the texture should be (if applicable)
	uint32_t history;
};

struct Pass {
	/// The name of the pass, specified by a `#pragma name` in the shader (may be empty)
	std::string name;
	/// The path to the base `slang` file of the shader.
	std::string path;
	/// String to be compiled for the vertex shader
	std::string vs_glsl;
	/// String to be compiled for the fragment shader
	std::string fs_glsl;
	/// Externally-compiled vertex shader (must contain debug info)
	std::vector<uint32_t> vs_spirv;
	/// Externally-compiled fragment shader (must contain debug info)
	std::vector<uint32_t> fs_spirv;
	/// List of parameters declared by this pass using `#pragma parameter` (indices into the filter chain's `parameters` array)
	std::vector<uint32_t> parameters;
	/// Information about the push constant binding for the shaders
	BoundBuffer push;
	/// Information about the UBO binding for the shaders
	BoundBuffer ubo;
	/// Textures bound to the shaders
	std::vector<BoundTexture> bindings;
	/// Wrap mode for this pass
	WrapMode wrap = WrapMode::ClampToBorder;
	/// Filter for this pass
	bool filter_linear = false;
	/// Format this pass should be rendered to
	TextureFormat out_format = TextureFormat::R8G8B8A8_UNORM;
	/// Whether to generate mipmaps for the pass preceding this one
	bool mipmap_input = false;
	/// Whether to generate mipmaps for this pass (calculated from `mipmap_input` for your convenience)
	bool mipmap_output = false;
	/// The scale type for the output framebuffer in the x direction
	ScaleType scale_type_x = ScaleType::Input;
	/// The scale type for the output framebuffer in the y direction
	ScaleType scale_type_y = ScaleType::Input;
	/// The multiplier for the x direction scale
	float scale_x = 1;
	/// The multiplier for the y direction scale
	float scale_y = 1;
	/// If nonzero, a modulus for buffer elements with type `FrameCount`
	uint32_t frame_count_mod = 0;
};

struct Parameter {
	std::string name;
	std::string description;
	float initial = 0;
	float minimum = 0;
	float maximum = 0;
	float step = 0;
	float value = 0;   ///< If `set`, the value that was set, otherwise `initial`
	bool set = false;  ///< If true, the parameter has had its `value` supplied by a `.slangp` file
	bool used = false; ///< If true, the parameter is referenced in the UBO or push constant declaration of a shader
	bool declared = false; ///< If true, the parameter was declared with a `#pragma parameter` (rather than just set in a slangp)
};

struct Texture {
	std::string name;
	std::string path;
	bool linear = false;
	bool mipmap = false;
	bool used = false; ///< If true, the texture is referenced by a shader
	WrapMode wrap = WrapMode::ClampToBorder;
};

struct FilterChain {
	/// List of passes
	std::vector<Pass> passes;
	/// List of textures specified by the slangp
	std::vector<Texture> textures;
	/// List of parameters referenced by the slangp or any of its shaders
	std::vector<Parameter> parameters;
	/// If the slangp has a `parameters = "..."` line, a list of indices into `parameters` representing that
	std::vector<uint32_t> explicit_parameters;
	/// The first `num_explicit_textures` textures in `textures` were declared in the slangp's `textures = "..."` line.  The rest were not.
	uint32_t num_explicit_textures = 0;
	/// The first `num_declared_parameters` parameters in `parameters` were declared by pragmas in a shader, and have a full definition.  The rest were only declared by the slangp.`
	uint32_t num_declared_parameters = 0;
	/// Does a quick check over the filter chain to verify all passes have shaders specified
	bool ok() const;
};

class ExecutionPlanner {
public:
	struct RT {
		TextureFormat format;
		bool mipmapped;
		ScaleType scale_type_x;
		ScaleType scale_type_y;
		float scale_x;
		float scale_y;
		static constexpr uint32_t PASS_SHARED = UINT32_MAX;
		/// The index of the one pass that uses this RT, or `PASS_SHARED` if it's used by multiple
		uint32_t pass;
	};
	struct Pass {
		/// Before this pass, transition the previous n pass' outputs to read access
		uint32_t num_transition_to_read;
		/// Before this pass, transition the next n pass' outputs to write access (1 means this pass's output, 2 means this and the next, etc)
		uint32_t num_transition_to_write;
		/// How many previous frames' versions of this pass's result need to be held on to
		uint32_t num_history_needed;
		/// The index first RT in the list of RTs used by this pass
		uint32_t rt_base;
		/// The index of the RT containing this pass's most recent output.
		uint32_t rt_current;
	};

private:
	std::vector<RT> rts;
	std::vector<Pass> passes;
	uint64_t frame_no_;
	uint32_t num_final_transition_to_read_ = 0;
	uint32_t input_history_needed_;
	uint32_t num_shared_rts_;
	bool output_direct_;
	bool uses_pass_history;

public:
	ExecutionPlanner() = default;
	explicit ExecutionPlanner(const FilterChain& filter, bool supports_output_direct = true);
	/// Advance to the next frame
	void next_frame();
	/// Get rt info
	const RT& rt(size_t idx) const { return rts[idx]; }
	/// Get pass info
	const Pass& pass(size_t idx) const { return passes[idx]; }
	/// Get the RT index of the given pass's output from `history` frames ago
	uint32_t get_pass_output(uint32_t pass, uint32_t history = 0) const;
	/// Get the current frame number
	uint64_t frame_no() const { return frame_no_; }
	/// Get the total number of RTs
	size_t num_rts() const { return rts.size(); }
	/// After running the last pass and before the next frame, transition the last n pass' outputs to read access
	uint32_t num_final_transition_to_read() const { return num_final_transition_to_read_; }
	/// The number of frames of history of the input frame need to be saved to run the filter chain
	uint32_t input_history_needed() const { return input_history_needed_; }
	/// RTs 0-n are shared, while the rest are used exclusively by a single pass
	uint32_t num_shared_rts() const { return num_shared_rts_; }
	/// If true, the displayed framebuffer can be used as the RT of the final pass.  In this case, `passes.back().rt_base` will be `UINT32_MAX`.
	bool output_direct() const { return output_direct_; }
};

// Expected API Usage:
// 1. Use parse_slangp or parse_slang to parse a filter chain
// 2. Loop over all passes in the filter chain, and compile their vs and fs to SPIR-V
// 3. Call parse_spirv on the filter chain to parse reflection information from the SPIR-V
// Errors are usually recoverable, so don't fail unless the resulting glsl shaders fail to compile

FilterChain parse_slangp(const char* path, std::vector<Error>* errors, const SlangParseConfig& config, FileReadCallback read = default_file_read_callback, void *read_ctx = nullptr);
FilterChain parse_slang(const char* path, std::vector<Error>* errors, const SlangParseConfig& config, FileReadCallback read = default_file_read_callback, void *read_ctx = nullptr);
void parse_slang(FilterChain& dst_chain, Pass& dst_pass, const char* path, std::vector<Error>* errors, const SlangParseConfig& config, FileReadCallback read = default_file_read_callback, void *read_ctx = nullptr);
bool parse_spirv(FilterChain& chain, std::vector<SPIRVError>* errors);
bool parse_spirv(FilterChain& chain, size_t pass, std::vector<SPIRVError>* errors);

} // namespace rashader

const char* to_string(rashader::ScaleType);
const char* to_string(rashader::TextureFormat);
const char* to_string(rashader::WrapMode);
const char* to_string(rashader::TextureSource);
const char* to_string(rashader::BufferElementType);
