// SPDX-FileCopyrightText: 2026 PCSX2 Dev Team
// SPDX-License-Identifier: MIT

#include <rashader-lite.h>
#include "common.h"
#include <cassert>
#include <cstring>
#include <vector>

template <typename T>
static bool bitequal(const T& a, const T& b) {
	return 0 == memcmp(&a, &b, sizeof(T));
}

static bool compatible(const rashader::Pass& pass, const rashader::ExecutionPlanner::RT& rt) {
	bool ok = true;
	ok &= pass.out_format == rt.format;
	ok &= pass.mipmap_output == rt.mipmapped;
	ok &= pass.scale_type_x == rt.scale_type_x;
	ok &= pass.scale_type_x == rt.scale_type_x;
	ok &= bitequal(pass.scale_x, rt.scale_x);
	ok &= bitequal(pass.scale_y, rt.scale_y);
	return ok;
}

static void assign(rashader::ExecutionPlanner::RT& rt, const rashader::Pass& pass, size_t pass_idx) {
	rt.pass = static_cast<uint32_t>(pass_idx);
	rt.format = pass.out_format;
	rt.mipmapped = pass.mipmap_output;
	rt.scale_type_x = pass.scale_type_x;
	rt.scale_type_y = pass.scale_type_y;
	rt.scale_x = pass.scale_x;
	rt.scale_y = pass.scale_y;
}

// To make our tests consistent across different C++ stdlibs, implement our own heap functions

template <typename T>
static void sift_down(T* heap, size_t elem, size_t size) {
	size_t child;
	while ((child = elem * 2 + 1) < size) {
		if (child + 1 < size && heap[child] > heap[child + 1])
			++child;
		if (heap[elem] > heap[child]) {
			std::swap(heap[elem], heap[child]);
			elem = child;
		} else {
			break;
		}
	}
}

template <typename T>
static void sift_up(T* heap, size_t elem) {
	while (elem) {
		size_t parent = (elem - 1) / 2;
		if (heap[elem] < heap[parent]) {
			std::swap(heap[elem], heap[parent]);
			elem = parent;
		} else {
			break;
		}
	}
}

template <typename T>
static NOINLINE void heapify(T* heap, size_t size) {
	if (size < 2)
		return;
	size_t i = (size - 2) / 2 + 1;
	do {
		--i;
		sift_down(heap, i, size);
	} while (i);
}

template <typename T>
static NOINLINE void heap_push(T* heap, size_t size) {
	assert(size);
	if (size >= 2)
		sift_up(heap, size - 1);
}

template <typename T>
static NOINLINE void heap_pop(T* heap, size_t size) {
	assert(size);
	if (size >= 2) {
		std::swap(heap[0], heap[size - 1]);
		sift_down(heap, 0, size - 1);
	}
}

namespace rashader {
struct AllocatedTexture {
	uint32_t last_pass;
	uint32_t idx;
	bool operator<(const AllocatedTexture& other) const { return last_pass < other.last_pass; }
	bool operator>(const AllocatedTexture& other) const { return last_pass > other.last_pass; }
};
struct TextureAllocator {
	std::vector<ExecutionPlanner::RT> rts;
	std::vector<uint32_t> rt_wrap;
	std::vector<uint32_t> free;
	std::vector<AllocatedTexture> allocated;
	TextureAllocator() {}
	uint32_t allocate_new(size_t pass_idx, uint32_t last_pass, const rashader::Pass& pass) {
		uint32_t idx = static_cast<uint32_t>(rts.size());
		assign(rts.emplace_back(), pass, pass_idx);
		allocated.push_back({ last_pass, idx });
		return idx;
	}
	uint32_t allocate_wrap(size_t pass_idx, uint32_t last_pass, const rashader::Pass& pass) {
		assert(rt_wrap.size() == rts.size());
		rt_wrap.emplace_back(static_cast<uint32_t>(pass_idx));
		return allocate_new(pass_idx, last_pass, pass);
	}
	uint32_t allocate(size_t pass_idx, uint32_t last_pass, const rashader::Pass& pass) {
		for (auto it = free.begin(), end = free.end(); it < end; ++it) {
			uint32_t rt_idx = *it;
			if (!compatible(pass, rts[rt_idx]))
				continue;
			if (rt_idx < rt_wrap.size() && last_pass >= rt_wrap[rt_idx])
				continue; // Texture is still needed when it gets overwritten by the starting half of the wrapping usage
			free.erase(it);
			allocated.push_back({ static_cast<uint32_t>(last_pass), rt_idx });
			rts[rt_idx].pass = ExecutionPlanner::RT::PASS_SHARED;
			return rt_idx;
		}
		return allocate_new(pass_idx, last_pass, pass);
	}
	void finish_pass(uint32_t pass_idx) {
		while (!allocated.empty() && allocated.front().last_pass == pass_idx) {
			free.push_back(allocated.front().idx);
			heap_pop(allocated.data(), allocated.size());
			allocated.pop_back();
		}
		assert(allocated.empty() || allocated.front().last_pass > pass_idx);
	}
};
enum TextureState : uint8_t {
	Read = 0,
	ReadNeeded,
	Write,
};
} // namespace rashader

rashader::ExecutionPlanner::ExecutionPlanner(const FilterChain& chain, bool supports_output_direct)
	: frame_no_(UINT64_MAX), input_history_needed_(0), output_direct_(false)
{
	size_t num_passes = static_cast<uint32_t>(chain.passes.size());
	size_t num_rt_passes = num_passes;
	std::vector<uint32_t> last_tex_usage(num_passes, 0);
	passes.resize(num_passes);
	TextureAllocator allocator;

	// Calculate history needs for each pass
	for (size_t pass_idx = 0; pass_idx < num_passes; pass_idx++) {
		const rashader::Pass& pass = chain.passes[pass_idx];
		for (const BoundTexture& tex : pass.bindings) {
			if (tex.source == TextureSource::Input && tex.history > input_history_needed_)
				input_history_needed_ = tex.history;
			if (tex.source != TextureSource::Pass)
				continue;
			uint32_t history = tex.history;
			assert(history || tex.index < pass_idx);
			if (history && tex.index > pass_idx)
				--history;
			if (passes[tex.index].num_history_needed < history) {
				passes[tex.index].num_history_needed = history;
				last_tex_usage[tex.index] = 0;
			}
			uint32_t usage_idx = static_cast<uint32_t>(tex.index > pass_idx ? pass_idx + chain.passes.size() : pass_idx);
			if (history == passes[tex.index].num_history_needed && usage_idx > last_tex_usage[tex.index])
				last_tex_usage[tex.index] = usage_idx;
		}
	}

	if (supports_output_direct && num_passes && !passes[num_passes - 1].num_history_needed && last_tex_usage[num_passes - 1] < num_passes) {
		const rashader::Pass& last = chain.passes[num_passes - 1];
		if (last.scale_type_x == ScaleType::Viewport && last.scale_type_y == ScaleType::Viewport && last.scale_x == 1 && last.scale_y == 1) {
			output_direct_ = true;
			--num_rt_passes;
			passes[num_passes - 1].rt_base = UINT32_MAX;
			passes[num_passes - 1].rt_current = UINT32_MAX;
		}
	}

	// Allocate passes that wrap around
	for (size_t pass_idx = 0; pass_idx < num_passes; pass_idx++) {
		if (passes[pass_idx].num_history_needed == 0 && last_tex_usage[pass_idx] >= num_passes) {
			uint32_t last = last_tex_usage[pass_idx] - static_cast<uint32_t>(num_passes);
			passes[pass_idx].rt_base = allocator.allocate_wrap(pass_idx, last, chain.passes[pass_idx]);
		}
	}
	heapify(allocator.allocated.data(), allocator.allocated.size());
	// Then allocate passes that don't wrap
	for (size_t pass_idx = 0; pass_idx < num_rt_passes; pass_idx++) {
		if (passes[pass_idx].num_history_needed == 0 && last_tex_usage[pass_idx] < num_passes) {
			uint32_t last_usage = last_tex_usage[pass_idx];
			passes[pass_idx].rt_base = allocator.allocate(pass_idx, last_usage ? last_usage : static_cast<uint32_t>(pass_idx), chain.passes[pass_idx]);
			if (last_usage == 0) {
				allocator.free.push_back(allocator.allocated.back().idx);
				allocator.allocated.pop_back();
			} else {
				heap_push(allocator.allocated.data(), allocator.allocated.size());
			}
		}
		allocator.finish_pass(static_cast<uint32_t>(pass_idx));
	}

	// To make things look nicer, put all the shared textures first
	size_t total_rts = allocator.rts.size(), single_idx = 0;
	for (const RT& rt : allocator.rts)
		single_idx += rt.pass == RT::PASS_SHARED;
	for (const Pass& pass : passes)
		total_rts += pass.num_history_needed ? pass.num_history_needed + 1 : 0;
	rts.reserve(total_rts);
	rts.resize(allocator.rts.size());
	std::vector<uint32_t> remap(allocator.rts.size());
	size_t multi_idx = 0;
	uses_pass_history = total_rts != rts.size();
	num_shared_rts_ = static_cast<uint32_t>(single_idx);
	for (size_t i = 0; i < remap.size(); i++) {
		const RT& rt = allocator.rts[i];
		size_t& idx = rt.pass == RT::PASS_SHARED ? multi_idx : single_idx;
		remap[i] = static_cast<uint32_t>(idx);
		rts[idx] = allocator.rts[i];
		++idx;
	}
	assert(single_idx == rts.size());
	assert(multi_idx == num_shared_rts_);
	for (size_t i = 0; i < num_rt_passes; i++)
		if (passes[i].num_history_needed == 0)
			passes[i].rt_current = passes[i].rt_base = remap[passes[i].rt_base];

	// Now add all the RTs for textures with history
	for (size_t i = 0; i < num_rt_passes; i++) {
		if (passes[i].num_history_needed) {
			size_t count = passes[i].num_history_needed + 1;
			passes[i].rt_current = passes[i].rt_base = static_cast<uint32_t>(rts.size());
			for (size_t j = 0; j < count; j++)
				assign(rts.emplace_back(), chain.passes[i], i);
		}
	}
	assert(total_rts == rts.size());

	// Calculate barriers
	// We ensure all textures are in read state to start
	std::vector<bool> needed(rts.size(), false);
	std::vector<AllocatedTexture> active;
	for (size_t i = 0; i < num_rt_passes; i++) {
		if (last_tex_usage[i] >= num_passes) {
			needed[passes[i].rt_base] = true;
			active.push_back({ last_tex_usage[i] - static_cast<uint32_t>(num_passes), passes[i].rt_base });
		}
	}
	heapify(active.data(), active.size());
	uint32_t next_transition_write = 0;
	size_t next_transition_read = 0;
	size_t last_write_transition = SIZE_MAX;
	for (size_t i = 0; i < num_passes; i++) {
		bool needs_transition = next_transition_write <= i;
		for (const BoundTexture& tex : chain.passes[i].bindings) {
			if (tex.source == TextureSource::Pass && tex.history == 0 && next_transition_read <= tex.index)
				needs_transition = true;
		}
		if (needs_transition) {
			// Read transitions
			passes[i].num_transition_to_read = static_cast<uint32_t>(i - next_transition_read);
			next_transition_read = i;

			// Write transitions
			size_t first = next_transition_write;
			while (next_transition_write < num_rt_passes && !needed[passes[next_transition_write].rt_base]) {
				size_t j = next_transition_write;
				if (passes[j].num_history_needed == 0 && last_tex_usage[j] > j && last_tex_usage[j] < num_passes) {
					needed[passes[j].rt_base] = true;
					active.push_back({ last_tex_usage[j], passes[j].rt_base });
					heap_push(active.data(), active.size());
				}
				++next_transition_write;
			}
			passes[i].num_transition_to_write = static_cast<uint32_t>(next_transition_write - i);
			if (next_transition_write != i) {
				if (last_write_transition != SIZE_MAX) {
					passes[last_write_transition].num_transition_to_write = static_cast<uint32_t>(i - last_write_transition);
				}
				last_write_transition = i;
			}
			assert(i >= num_rt_passes || next_transition_write > i);
		}
		while (!active.empty() && active.front().last_pass == i) {
			assert(needed[active.front().idx]);
			needed[active.front().idx] = false;
			heap_pop(active.data(), active.size());
			active.pop_back();
		}
		assert(active.empty() || active.front().last_pass > i);
	}
	num_final_transition_to_read_ = static_cast<uint32_t>(num_passes - next_transition_read);
}

uint32_t rashader::ExecutionPlanner::get_pass_output(uint32_t pass, uint32_t history) const {
	if (history > frame_no_)
		history = static_cast<uint32_t>(frame_no_);
	uint32_t base    = passes[pass].rt_base;
	uint32_t current = passes[pass].rt_current;
	uint32_t max     = passes[pass].num_history_needed;
	uint32_t count   = max + 1;
	// If a shader uses the size of a history texture but not the texture itself, we may optimize it out.
	// In that case, give a reference to the current texture (which will be the same size unless the viewport was just resized, in which case too bad).
	// We can change it if someone gives an actual reason why it would be useful to know the size of a history texture without sampling it.
	// (We currently run dead texture elimination but not dead UBO entry elimination, so more likely this will happen for unused texture+size.)
	if (history > max)
		history = max;
	if (current - base < history)
		current += count;
	return current - history;
}

void rashader::ExecutionPlanner::next_frame() {
	++frame_no_;
	if (uses_pass_history) {
		for (Pass& pass : passes) {
			if (!pass.num_history_needed)
				continue;
			bool wrap = pass.rt_current == pass.rt_base + pass.num_history_needed;
			pass.rt_current = wrap ? pass.rt_base : pass.rt_current + 1;
		}
	}
}
