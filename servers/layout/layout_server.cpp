/**************************************************************************/
/*  layout_server.cpp                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "layout_server.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "scene/gui/container.h"
#include "scene/gui/control.h"
#include "scene/scene_string_names.h"

LayoutServer *LayoutServer::singleton = nullptr;

#define FETCH_CACHE(var, rid) \
	if (rid.is_null()) { \
		return; \
	} \
	ERR_FAIL_COND_MSG(!cache_owner.owns(rid), "ControlLayoutCache does not own the RID: " + itos(rid.get_id())); \
	ControlLayoutCache *var = cache_owner.get_or_null(rid); \
	ERR_FAIL_NULL_MSG(var, "ControlLayoutCache not found for RID: " + itos(rid.get_id()));

#define FETCH_CACHE_V(var, rid, ret) \
	if (rid.is_null()) { \
		return ret; \
	} \
	ERR_FAIL_COND_V_MSG(!cache_owner.owns(rid), ret, "ControlLayoutCache does not own the RID: " + itos(rid.get_id())); \
	ControlLayoutCache *var = cache_owner.get_or_null(rid); \
	ERR_FAIL_NULL_V_MSG(var, ret, "ControlLayoutCache not found for RID: " + itos(rid.get_id()));

#define NULL_RID(rid) \
	if (rid.is_null()) { \
		return; \
	}

#define FETCH_BUCKET(depth) \
	LayoutBucket *bucket = nullptr; \
	if (dirty_buckets.has(depth)) { \
		bucket = dirty_buckets[depth]; \
	} else { \
		bucket = memnew(LayoutBucket()); \
		dirty_buckets[depth] = bucket; \
	}

#define UPDATE_DIRTY_DEPTH(depth) \
	if (depth > max_dirty_depth) { \
		max_dirty_depth = depth; \
	}

/// Lifecycle management.

RID LayoutServer::add_control(Control *p_control) {
	ERR_FAIL_COND_V_MSG(is_doing_layout, RID(), "Attempting to modify control tree (adding new control) while doing layout. Use call_deferred instead.");
	RID control_rid = cache_owner.make_rid();

	ControlLayoutCache *cache = cache_owner.get_or_null(control_rid);
	ERR_FAIL_NULL_V_MSG(cache, RID(), "ControlLayoutCache allocation failed.");

	cache->self = control_rid;
	cache->control = p_control;

	return control_rid;
}

void LayoutServer::free_control(RID p_rid) {
	ERR_FAIL_COND_MSG(is_doing_layout, "Attempting to modify control tree (freeing control) while doing layout. Use call_deferred instead.");
	FETCH_CACHE(cache, p_rid);

	ControlLayoutCache *parent_cache = cache_owner.get_or_null(cache->parent);
	if (parent_cache) {
		parent_cache->children.erase(p_rid);
		if (cache->layout_mode == LAYOUT_MODE_CONTAINER) {
			update_minimum_size(cache->parent);
			update_desired_size(cache->parent);
			update_layout(cache->parent);
		}
	}

	redraw_queue.erase(p_rid);
	cache_owner.free(p_rid);
}

/// Tree management.

void LayoutServer::set_control_parent(RID p_rid, RID p_parent) {
	ERR_FAIL_COND_MSG(is_doing_layout, "Attempting to modify control tree (setting parent) while doing layout. Use call_deferred instead.");
	FETCH_CACHE(cache, p_rid);

	if (cache->parent == p_parent) {
		return;
	}

	print_line_debug("Setting Control ", p_rid.get_id(), " parent to ", p_parent.get_id());

	ControlLayoutCache *parent_cache = cache_owner.get_or_null(cache->parent);
	if (parent_cache) {
		parent_cache->children.erase(p_rid);
		update_minimum_size(cache->parent);
		update_desired_size(cache->parent);
		update_layout(cache->parent);
		queue_redraw(cache->parent);
	}

	cache->parent = p_parent;
	cache->layout_mode = get_default_layout_mode(p_rid);

	parent_cache = cache_owner.get_or_null(p_parent);
	if (parent_cache) {
		parent_cache->children.push_back(p_rid);
		update_minimum_size(p_parent);
		update_desired_size(p_parent);
		update_layout(p_parent);
		queue_redraw(p_parent);
		set_control_depth(p_rid, parent_cache->depth + 1);
	} else {
		set_control_depth(p_rid, 0);
	}

	update_maximum_size(p_rid);
}

void LayoutServer::set_control_depth(RID p_rid, int p_depth) {
	ERR_FAIL_COND_MSG(is_doing_layout, "Attempting to modify control tree (setting depth) while doing layout. Use call_deferred instead.");
	FETCH_CACHE(cache, p_rid);

	if ((int)cache->depth == p_depth) {
		return;
	}

	if (dirty_buckets.has(cache->depth)) {
		dirty_buckets[cache->depth]->erase(p_rid);
	}

	FETCH_BUCKET(p_depth);
	UPDATE_DIRTY_DEPTH(p_depth);

	if (cache->dirty_flags.has_flag(DIRTY_MIN_SIZE)) {
		bucket->dirty_min_sizes.push_back(p_rid);
	}
	if (cache->dirty_flags.has_flag(DIRTY_MAX_SIZE)) {
		bucket->dirty_max_sizes.push_back(p_rid);
	}
	if (cache->dirty_flags.has_flag(DIRTY_LAYOUT)) {
		bucket->dirty_layouts.push_back(p_rid);
	}

	cache->depth = p_depth;
	for (RID child : cache->children) {
		set_control_depth(child, p_depth + 1);
	}
}

void LayoutServer::set_control_as_internal_child(RID p_rid, bool p_internal) {
	FETCH_CACHE(cache, p_rid);
	cache->is_internal_child = p_internal;
}

/// State management.

void LayoutServer::update_visibility(RID p_rid) {
	if (p_rid.is_null()) {
		return;
	}

	FETCH_CACHE(cache, p_rid);

	if (cache->parent == RID() || cache->layout_mode != LAYOUT_MODE_CONTAINER || cache->is_internal_child) {
		return;
	}

	if (is_doing_layout) {
		WARN_PRINT("Attempting to update visibility for a contained child while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &LayoutServer::update_visibility).call_deferred(p_rid);
		return;
	}

	update_minimum_size(cache->parent);
	update_desired_size(cache->parent);
	update_layout(cache->parent);
}

void LayoutServer::update_layout(RID p_rid) {
	if (is_doing_layout) {
		WARN_PRINT("Attempting to update layout while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &LayoutServer::update_layout).call_deferred(p_rid);
		return;
	}
	_update_layout(p_rid);
}

void LayoutServer::_update_layout(RID p_rid) {
	FETCH_CACHE(cache, p_rid);

	if (cache->dirty_flags.has_flag(DIRTY_LAYOUT)) {
		return;
	}

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->dirty_layouts.push_back(p_rid);
	cache->dirty_flags.set_flag(DIRTY_LAYOUT);
}

void LayoutServer::update_parent_layout(RID p_rid) {
	if (p_rid.is_null()) {
		return;
	}

	FETCH_CACHE(cache, p_rid);

	if (cache->parent.is_null() || cache->layout_mode != LAYOUT_MODE_CONTAINER) {
		return;
	}

	update_layout(cache->parent);
}

void LayoutServer::update_minimum_size(RID p_rid) {
	if (is_doing_layout) {
		WARN_PRINT("Attempting to update minimum size while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &LayoutServer::update_minimum_size).call_deferred(p_rid);
		return;
	}
	_update_minimum_size(p_rid);
}

void LayoutServer::_update_minimum_size(RID p_rid) {
	NULL_RID(p_rid);
	FETCH_CACHE(cache, p_rid);

	if (cache->dirty_flags.has_flag(DIRTY_MIN_SIZE)) {
		return;
	}

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->dirty_min_sizes.push_back(p_rid);
	cache->dirty_flags.set_flag(DIRTY_MIN_SIZE);
}

bool LayoutServer::_update_min_size_cache(RID p_rid) {
	FETCH_CACHE_V(cache, p_rid, false);

	Control *control = cache->control;
	if (!control->is_inside_tree()) {
		return false;
	}

	Size2 minsize = INVALID_SIZE;

	if (control->is_visible_in_tree()) {
		minsize = control->get_minimum_size();
	}

	minsize = minsize.max(cache->custom_min_size);

	bool updated = cache->combined_min_size != minsize;
	cache->combined_min_size = minsize;
	cache->dirty_flags.clear_flag(DIRTY_MIN_SIZE);
	control->emit_signal(SceneStringName(minimum_size_changed));
	return updated;
}

void LayoutServer::update_maximum_size(RID p_rid) {
	if (is_doing_layout) {
		WARN_PRINT("Attempting to update maximum size while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &LayoutServer::update_maximum_size).call_deferred(p_rid);
		return;
	};
	_update_maximum_size(p_rid);
}

void LayoutServer::_update_maximum_size(RID p_rid) {
	NULL_RID(p_rid);
	FETCH_CACHE(cache, p_rid);

	if (cache->dirty_flags.has_flag(DIRTY_MAX_SIZE)) {
		return;
	}

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->dirty_max_sizes.push_back(p_rid);
	cache->dirty_flags.set_flag(DIRTY_MAX_SIZE);
}

bool LayoutServer::_update_max_size_cache(RID p_rid) {
	FETCH_CACHE_V(cache, p_rid, false);

	Control *control = cache->control;
	if (!control->is_inside_tree()) {
		return false;
	}

	Size2 maxsize = INVALID_SIZE;

	if (control->is_visible_in_tree()) {
		maxsize = control->get_maximum_size();
		Size2 custom_max_size = cache->custom_max_size;
		Size2 parent_maximum_size_cache = cache->parent_maximum_size_cache;

		if (custom_max_size.x >= 0) {
			if (maxsize.x >= 0) {
				maxsize.x = MIN(maxsize.x, custom_max_size.x);
			} else {
				maxsize.x = custom_max_size.x;
			}
		}
		if (custom_max_size.y >= 0) {
			if (maxsize.y >= 0) {
				maxsize.y = MIN(maxsize.y, custom_max_size.y);
			} else {
				maxsize.y = custom_max_size.y;
			}
		}

		if (parent_maximum_size_cache.x >= 0) {
			if (maxsize.x >= 0) {
				maxsize.x = MIN(maxsize.x, parent_maximum_size_cache.x);
			} else {
				maxsize.x = parent_maximum_size_cache.x;
			}
		}
		if (parent_maximum_size_cache.y >= 0) {
			if (maxsize.y >= 0) {
				maxsize.y = MIN(maxsize.y, parent_maximum_size_cache.y);
			} else {
				maxsize.y = parent_maximum_size_cache.y;
			}
		}
	}

	bool updated = cache->combined_max_size != maxsize;
	cache->combined_max_size = maxsize;
	cache->inner_combined_max_size = control->get_inner_combined_maximum_size().min(maxsize);
	cache->dirty_flags.clear_flag(DIRTY_MAX_SIZE);
	control->emit_signal(SceneStringName(maximum_size_changed));
	return updated;
}

void LayoutServer::update_desired_size(RID p_rid) {
	if (is_doing_layout) {
		WARN_PRINT("Attempting to update desired size while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &LayoutServer::update_desired_size).call_deferred(p_rid);
		return;
	};
	_update_desired_size(p_rid);
}

void LayoutServer::_update_desired_size(RID p_rid) {
	NULL_RID(p_rid);
	FETCH_CACHE(cache, p_rid);

	cache->desired_size_cache[0].clear();
	cache->desired_size_cache[1].clear();

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->dirty_layouts.push_back(p_rid);
}

bool LayoutServer::_update_desired_size_for_axis(RID p_rid, Axis p_axis, real_t p_for_size) {
	FETCH_CACHE_V(cache, p_rid, false);

	real_t desired_size = -1;
	if (cache->desired_size_cache[(int)p_axis].has(p_for_size)) {
		desired_size = cache->desired_size_cache[(int)p_axis][p_for_size];
	}

	real_t new_desired_size = _get_desired_size_for_axis(p_rid, p_axis, p_for_size);
	return !Math::is_equal_approx(desired_size, new_desired_size);
}

void LayoutServer::queue_redraw(RID p_rid) {
	FETCH_CACHE(cache, p_rid);
	if (cache->dirty_flags.has_flag(DIRTY_DRAW)) {
		return;
	}
	cache->dirty_flags.set_flag(DIRTY_DRAW);
	redraw_queue.push_back(p_rid);
}

/// Internal Position and Sizing methods

real_t LayoutServer::_get_desired_size_for_axis(RID p_rid, Axis p_axis, real_t p_for_size) const {
	FETCH_CACHE_V(cache, p_rid, 0.0);

	if (cache->desired_size_cache[(int)p_axis].has(p_for_size)) {
		return cache->desired_size_cache[(int)p_axis][p_for_size];
	}

	Control *control = cache->control;
	real_t measured_size = control->get_desired_size(p_axis, p_for_size);

	cache->desired_size_cache[(int)p_axis][p_for_size] = measured_size;

	return measured_size;
}

// FIXME: Figure out if we can't fold this method into `Control`.
// Currently used by some `CanvasItem`s that could realistically be better off as `Control`? And deprecate the others since they don't make sense for UI positioning.
// Full list (July 2026): Sprite2D, AnimatedSprite2D, PointLight2D (???), TouchScreenButton (docs say this one doesn't even support...), BackBufferCopy (docs say this one doesn't even support...)
Rect2 LayoutServer::_get_parent_anchorable_rect(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, Rect2());
	Control *control = cache->control;
	return control->get_parent_anchorable_rect();
}

void LayoutServer::_compute_layout_rect(RID p_rid, Rect2 p_rect, bool p_keep_offsets) {
	FETCH_CACHE(cache, p_rid);

	Size2 parent_rect_size = _get_parent_anchorable_rect(p_rid).size;
	if (p_keep_offsets) {
		// If computing anchors, we need to ensure the parent rect size is valid to avoid division by zero.
		ERR_FAIL_COND(parent_rect_size.x == 0.0);
		ERR_FAIL_COND(parent_rect_size.y == 0.0);
	}

	Size2 max_size = cache->combined_max_size;
	Size2 min_size = cache->combined_min_size;

	Rect2 clamped_rect = p_rect;
	clamped_rect.size = clamped_rect.size.max(min_size);

	if (max_size.x >= 0) {
		clamped_rect.size.x = MIN(clamped_rect.size.x, max_size.x);
	}
	if (max_size.y >= 0) {
		clamped_rect.size.y = MIN(clamped_rect.size.y, max_size.y);
	}

	if (cache->layout_mode != LayoutMode::LAYOUT_MODE_CONTAINER) {
		float left_grow_factor =
				(cache->h_grow == GROW_DIRECTION_BEGIN) ? 1.0f
				: (cache->h_grow == GROW_DIRECTION_END) ? 0.0f
														: 0.5f;
		float top_grow_factor =
				(cache->v_grow == GROW_DIRECTION_BEGIN) ? 1.0f
				: (cache->v_grow == GROW_DIRECTION_END) ? 0.0f
														: 0.5f;

		Size2 size_diff = p_rect.size - get_proposed_size(p_rid);

		p_rect.position.x -= size_diff.x * (cache->control->is_layout_rtl() ? (1.0f - left_grow_factor) : left_grow_factor);
		p_rect.position.y -= size_diff.y * top_grow_factor;
	}

	if (cache->control->is_layout_rtl()) {
		p_rect.position.x = parent_rect_size.x - p_rect.position.x - p_rect.size.x;
	}

	if (p_keep_offsets) {
		cache->proposed_anchors[0] = (p_rect.position.x - cache->proposed_offsets[0]) / parent_rect_size.x;
		cache->proposed_anchors[1] = (p_rect.position.y - cache->proposed_offsets[1]) / parent_rect_size.y;
		cache->proposed_anchors[2] = (p_rect.position.x + p_rect.size.x - cache->proposed_offsets[2]) / parent_rect_size.x;
		cache->proposed_anchors[3] = (p_rect.position.y + p_rect.size.y - cache->proposed_offsets[3]) / parent_rect_size.y;
	} else {
		cache->proposed_offsets[0] = p_rect.position.x - (cache->proposed_anchors[0] * parent_rect_size.x);
		cache->proposed_offsets[1] = p_rect.position.y - (cache->proposed_anchors[1] * parent_rect_size.y);
		cache->proposed_offsets[2] = p_rect.position.x + p_rect.size.x - (cache->proposed_anchors[2] * parent_rect_size.x);
		cache->proposed_offsets[3] = p_rect.position.y + p_rect.size.y - (cache->proposed_anchors[3] * parent_rect_size.y);
	}
}

/// Positioning and Sizing API

void LayoutServer::set_default_children_layout_mode(RID p_rid, LayoutMode p_mode) {
	FETCH_CACHE(cache, p_rid);
	cache->default_children_layout_mode = p_mode;
}

LS::LayoutMode LayoutServer::get_default_children_layout_mode(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, LAYOUT_MODE_POSITION);
	return cache->default_children_layout_mode;
}

void LayoutServer::set_layout_mode(RID p_rid, LayoutMode p_mode) {
	FETCH_CACHE(cache, p_rid);
	cache->layout_mode = p_mode;
}

LS::LayoutMode LayoutServer::get_layout_mode(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, LAYOUT_MODE_POSITION);
	return cache->layout_mode;
}

LS::LayoutMode LayoutServer::get_default_layout_mode(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, LAYOUT_MODE_POSITION);
	ControlLayoutCache *parent = cache_owner.get_or_null(cache->parent);
	return parent ? parent->default_children_layout_mode : LAYOUT_MODE_POSITION;
}

void LayoutServer::set_h_grow_direction(RID p_rid, LS::GrowDirection p_direction) {
	FETCH_CACHE(cache, p_rid);
	cache->h_grow = p_direction;
}

LayoutServer::GrowDirection LayoutServer::get_h_grow_direction(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, GROW_DIRECTION_BEGIN);
	return cache->h_grow;
}

void LayoutServer::set_v_grow_direction(RID p_rid, LS::GrowDirection p_direction) {
	FETCH_CACHE(cache, p_rid);
	cache->v_grow = p_direction;
}

LayoutServer::GrowDirection LayoutServer::get_v_grow_direction(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, GROW_DIRECTION_BEGIN);
	return cache->v_grow;
}

void LayoutServer::set_size_request_mode(RID p_rid, SizeRequestMode p_mode) {
	FETCH_CACHE(cache, p_rid);
	cache->size_request_mode = p_mode;
}

LS::SizeRequestMode LayoutServer::get_size_request_mode(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, SIZE_REQUEST_CONSTANT);
	return cache->size_request_mode;
}

void LayoutServer::set_proposed_layout(RID p_parent_rid, RID p_child_rid, Axis p_axis, Vector2 p_proposed_layout) {
	FETCH_CACHE(cache, p_child_rid);
	ERR_FAIL_COND_MSG(cache->parent != p_parent_rid, "set_proposed_size can only be called for a child of the given parent.");
	FETCH_CACHE(parent_cache, p_parent_rid);
	ERR_FAIL_COND_MSG(p_parent_rid != proposing_rid, "set_proposed_size can only be called during the parent's children layout proposal phase.");

	Point2 proposed_position = get_proposed_position(p_child_rid);
	Size2 proposed_size = get_proposed_size(p_child_rid);

	if (Math::is_equal_approx(proposed_position[(int)p_axis], p_proposed_layout[0]) && Math::is_equal_approx(proposed_size[(int)p_axis], p_proposed_layout[1])) {
		return;
	}

	proposed_position[(int)p_axis] = p_proposed_layout[0];
	proposed_size[(int)p_axis] = p_proposed_layout[1];

	_compute_layout_rect(p_child_rid, Rect2(proposed_position, proposed_size), false);
	_update_layout(p_child_rid);
}

Vector4 LayoutServer::get_anchors(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, Vector4());
	return cache->anchors;
}

void LayoutServer::set_anchor(RID p_rid, Side p_side, real_t p_anchor, bool p_keep_offset, bool p_push_opposite_anchor) {
	FETCH_CACHE(cache, p_rid);

	if (p_anchor == cache->proposed_anchors[p_side]) {
		return;
	}

	Rect2 parent_rect = _get_parent_anchorable_rect(p_rid);
	real_t parent_range = (p_side == SIDE_LEFT || p_side == SIDE_RIGHT) ? parent_rect.size.x : parent_rect.size.y;
	real_t previous_pos = cache->proposed_offsets[p_side] + cache->proposed_anchors[p_side] * parent_range;
	real_t previous_opposite_pos = cache->proposed_offsets[(p_side + 2) % 4] + cache->proposed_anchors[(p_side + 2) % 4] * parent_range;

	cache->proposed_anchors[p_side] = p_anchor;

	if (((p_side == SIDE_LEFT || p_side == SIDE_TOP) && cache->proposed_anchors[p_side] > cache->proposed_anchors[(p_side + 2) % 4]) ||
			((p_side == SIDE_RIGHT || p_side == SIDE_BOTTOM) && cache->proposed_anchors[p_side] < cache->proposed_anchors[(p_side + 2) % 4])) {
		if (p_push_opposite_anchor) {
			cache->proposed_anchors[(p_side + 2) % 4] = cache->proposed_anchors[p_side];
		} else {
			cache->proposed_anchors[p_side] = cache->proposed_anchors[(p_side + 2) % 4];
		}
	}

	if (!p_keep_offset) {
		cache->proposed_offsets[p_side] = previous_pos - cache->proposed_anchors[p_side] * parent_range;
		if (p_push_opposite_anchor) {
			cache->proposed_offsets[(p_side + 2) % 4] = previous_opposite_pos - cache->proposed_anchors[(p_side + 2) % 4] * parent_range;
		}
	}
	// print_line_debug("Setting anchor for control: " + itos(p_rid.get_id()) + " side: " + itos(p_side) + " to: " + rtos(p_anchor) + ", keep_offset: " + itos(p_keep_offset) + ", push_opposite: " + itos(p_push_opposite_anchor));
	_update_layout(p_rid);
}

Vector4 LayoutServer::get_offsets(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, Vector4());
	return cache->offsets;
}

void LayoutServer::set_offset(RID p_rid, Side p_side, real_t p_value) {
	ERR_FAIL_COND(!std::isfinite(p_value));
	FETCH_CACHE(cache, p_rid);

	if (p_value == cache->proposed_offsets[p_side]) {
		return;
	}

	cache->proposed_offsets[p_side] = p_value;
	_update_layout(p_rid);
}

Size2 LayoutServer::get_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, INVALID_SIZE);
	Size2 parent_rect = _get_parent_anchorable_rect(p_rid).size;
	real_t width = parent_rect.width * (cache->anchors[SIDE_RIGHT] - cache->anchors[SIDE_LEFT]) + cache->offsets[SIDE_RIGHT] - cache->offsets[SIDE_LEFT];
	real_t height = parent_rect.height * (cache->anchors[SIDE_BOTTOM] - cache->anchors[SIDE_TOP]) + cache->offsets[SIDE_BOTTOM] - cache->offsets[SIDE_TOP];
	return Size2(width, height);
}

void LayoutServer::set_size(RID p_rid, Size2 p_size, bool p_keep_offsets) {
	FETCH_CACHE(cache, p_rid);
	print_line_debug("Setting size for control: ", p_rid.get_id(), " to: ", p_size, ", keep_offsets: ", p_keep_offsets, " current size: ", get_size(p_rid), " current anchors: ", cache->anchors, " current offsets: ", cache->offsets);

	Size2 new_size = p_size.max(get_combined_minimum_size(p_rid));
	Size2 max = get_combined_maximum_size(p_rid);
	if (max.x >= 0 && new_size.x > max.x) {
		new_size.x = max.x;
	}
	if (max.y >= 0 && new_size.y > max.y) {
		new_size.y = max.y;
	}

	print_line_debug("After clamping, new size: ", new_size, " min size: ", get_combined_minimum_size(p_rid), " max size: ", max);

	if (get_proposed_size(p_rid) == new_size) {
		return;
	}

	_compute_layout_rect(p_rid, Rect2(get_proposed_position(p_rid), new_size), p_keep_offsets);

	_update_layout(p_rid);

	// Notes: Currently, the user needs to make sure the minimum size is up to date before calling this, which may lead to some unexpected behavior in certain cases.
	// See test_tab_bar.cpp:"Ensure Tab Visible" for an example of how this can be a problem. The TabBar's set_clip_tabs needs to be processed before set_size is called.
	// TODO: Do the clamping after min/max updates & propagation.
}

Size2 LayoutServer::get_proposed_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, INVALID_SIZE);
	Size2 parent_rect = _get_parent_anchorable_rect(p_rid).size;
	real_t width = parent_rect.width * (cache->proposed_anchors[SIDE_RIGHT] - cache->proposed_anchors[SIDE_LEFT]) + cache->proposed_offsets[SIDE_RIGHT] - cache->proposed_offsets[SIDE_LEFT];
	real_t height = parent_rect.height * (cache->proposed_anchors[SIDE_BOTTOM] - cache->proposed_anchors[SIDE_TOP]) + cache->proposed_offsets[SIDE_BOTTOM] - cache->proposed_offsets[SIDE_TOP];
	return Size2(width, height);
}

Point2 LayoutServer::get_position(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, Point2());
	Rect2 parent_rect = _get_parent_anchorable_rect(p_rid);
	real_t h_pos = cache->anchors[SIDE_LEFT] * parent_rect.size.width + cache->offsets[SIDE_LEFT];
	real_t v_pos = cache->anchors[SIDE_TOP] * parent_rect.size.height + cache->offsets[SIDE_TOP];
	return Point2(h_pos, v_pos);
}

void LayoutServer::set_position(RID p_rid, Point2 p_position, bool p_keep_offsets) {
	FETCH_CACHE(cache, p_rid);
	// print_line_debug("Setting position for control: ", itos(p_rid.get_id()), " to: ", p_position);

	if (get_position(p_rid) == p_position) {
		return;
	}

	_compute_layout_rect(p_rid, Rect2(p_position, get_proposed_size(p_rid)), p_keep_offsets);

	_update_layout(p_rid);
}

Point2 LayoutServer::get_proposed_position(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, Point2());
	Rect2 parent_rect = _get_parent_anchorable_rect(p_rid);
	real_t h_pos = cache->proposed_anchors[SIDE_LEFT] * parent_rect.size.width + cache->proposed_offsets[SIDE_LEFT];
	real_t v_pos = cache->proposed_anchors[SIDE_TOP] * parent_rect.size.height + cache->proposed_offsets[SIDE_TOP];
	return Point2(h_pos, v_pos);
}

Size2 LayoutServer::get_custom_minimum_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, INVALID_SIZE);
	return cache->custom_min_size;
}

void LayoutServer::set_custom_minimum_size(RID p_rid, Size2 p_size) {
	FETCH_CACHE(cache, p_rid);
	if (!p_size.is_finite()) {
		return;
	}
	Size2 normalized = p_size.max(Size2());
	if (cache->custom_min_size == normalized) {
		return;
	}

	cache->custom_min_size = normalized;
	update_minimum_size(p_rid);
}

Size2 LayoutServer::get_custom_maximum_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, INVALID_SIZE);
	return cache->custom_max_size;
}

void LayoutServer::set_custom_maximum_size(RID p_rid, Size2 p_size) {
	FETCH_CACHE(cache, p_rid);
	if (cache->custom_max_size == p_size) {
		return;
	}
	if (!p_size.is_finite()) {
		return;
	}

	Size2 normalized = p_size;
	if (normalized.x < 0) {
		normalized.x = -1;
	}
	if (normalized.y < 0) {
		normalized.y = -1;
	}

	cache->custom_max_size = normalized;
	update_maximum_size(p_rid);
	update_minimum_size(p_rid);
	update_desired_size(p_rid);
}

Size2 LayoutServer::get_combined_minimum_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, INVALID_SIZE);
	return cache->combined_min_size;
}

Size2 LayoutServer::get_bound_minimum_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, INVALID_SIZE);
	Size2 min_size = cache->combined_min_size;
	Size2 max_size = cache->combined_max_size;

	if (max_size.x >= 0 && min_size.x > max_size.x) {
		min_size.x = max_size.x;
	}
	if (max_size.y >= 0 && min_size.y > max_size.y) {
		min_size.y = max_size.y;
	}

	return min_size;
}

Size2 LayoutServer::get_combined_maximum_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, INVALID_SIZE);
	return cache->combined_max_size;
}

bool LayoutServer::is_propagating_maximum_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, false);
	return cache->propagate_maximum_size;
}

void LayoutServer::set_propagate_maximum_size(RID p_rid, bool p_propagate) {
	FETCH_CACHE(cache, p_rid);
	if (cache->propagate_maximum_size == p_propagate) {
		return;
	}
	cache->propagate_maximum_size = p_propagate;
	cache->dirty_flags.set_flag(DIRTY_MAX_SIZE_PROPAGATION);
	update_maximum_size(p_rid);
}

void LayoutServer::set_parent_maximum_size_cache(RID p_rid, Size2 p_size) {
	FETCH_CACHE(cache, p_rid);
	if (!p_size.is_finite()) {
		return;
	}

	Size2 normalized = p_size;
	if (normalized.x < 0) {
		normalized.x = -1;
	}
	if (normalized.y < 0) {
		normalized.y = -1;
	}

	cache->parent_maximum_size_cache = normalized;
}

Size2 LayoutServer::get_desired_size(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, Size2());
	switch (cache->size_request_mode) {
		case SIZE_REQUEST_CONSTANT: {
			return Size2(cache->desired_size_cache[(int)Axis::AXIS_X][-1], cache->desired_size_cache[(int)Axis::AXIS_Y][-1]);
		}
		case SIZE_REQUEST_HEIGHT_FOR_WIDTH: {
			real_t width = get_size(p_rid).width;
			return Size2(width, _get_desired_size_for_axis(p_rid, Axis::AXIS_Y, width));
		}
		case SIZE_REQUEST_WIDTH_FOR_HEIGHT: {
			real_t height = get_size(p_rid).height;
			return Size2(_get_desired_size_for_axis(p_rid, Axis::AXIS_X, height), height);
		}
	};
}

bool LayoutServer::is_layout_dirty(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, false);
	return cache->dirty_flags.has_flag(DIRTY_LAYOUT) || cache->dirty_flags.has_flag(DIRTY_MIN_SIZE) || cache->dirty_flags.has_flag(DIRTY_MAX_SIZE) || cache->dirty_flags.has_flag(DIRTY_MAX_SIZE_PROPAGATION);
}

bool LayoutServer::is_layout_dirty_in_tree(RID p_rid) const {
	FETCH_CACHE_V(cache, p_rid, false);
	return is_layout_dirty(p_rid) || is_layout_dirty_in_tree(cache->parent);
}

void LayoutServer::add_post_layout_callback(const Callable &p_callable) {
	if (!p_callable.is_valid()) {
		return;
	}
	callback_queue.push_back(p_callable);
}

/// Main layout loop.

void LayoutServer::_desired_sizes_pass(Axis p_axis, bool p_monotonic_clamping) {
	Axis opposite_axis = (p_axis == Axis::AXIS_X) ? Axis::AXIS_Y : Axis::AXIS_X;
	for (int64_t depth = max_dirty_depth; depth >= 0; depth--) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		for (uint32_t i = 0; i < bucket->dirty_layouts.size(); i++) {
			RID rid = bucket->dirty_layouts[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				continue;
			}

			Size2 proposed_size = get_proposed_size(rid);
			bool updated = _update_desired_size_for_axis(rid, p_axis, proposed_size[(int)opposite_axis]);

			if (cache->layout_mode != LAYOUT_MODE_CONTAINER) {
				real_t desired_size = cache->desired_size_cache[(int)p_axis][proposed_size[(int)opposite_axis]];
				real_t clamped_size = p_monotonic_clamping ? MIN(proposed_size[(int)p_axis], desired_size) : desired_size;
				proposed_size[(int)p_axis] = clamped_size;
				_compute_layout_rect(rid, Rect2(get_proposed_position(rid), proposed_size), false);
			}

			if (!updated) {
				continue;
			}

			if (cache->parent != RID() && cache->layout_mode == LAYOUT_MODE_CONTAINER) {
				_update_desired_size(cache->parent);
				_update_layout(cache->parent);
			}
		}
	}
}

// Completes one constrained layout pass:
// First, proposes sizes down the tree for p_axis, then updates the opposite axis desired sizes up the tree.
void LayoutServer::_layout_pass(Axis p_axis, bool p_monotonic_clamping) {
	// First, propose sizes down the tree for p_axis
	for (int64_t depth = 0; depth <= max_dirty_depth; depth++) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		for (uint32_t i = 0; i < bucket->dirty_layouts.size(); i++) {
			RID rid = bucket->dirty_layouts[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache || cache->layout_mode != LAYOUT_MODE_CONTAINER) {
				continue;
			}
			proposing_rid = rid;
			cache->control->propose_children_layouts(p_axis, get_proposed_size(rid)[(int)p_axis]);
			proposing_rid = RID();
		}
	}

	// Then update the opposite axis desired sizes up the tree
	Axis opposite_axis = (p_axis == Axis::AXIS_X) ? Axis::AXIS_Y : Axis::AXIS_X;
	_desired_sizes_pass(opposite_axis, p_monotonic_clamping);
}

void LayoutServer::layout() {
	// Method called by Main which synchronizes the control layout.
	/** Method responsibilities:
	1. Perform layout calculations as needed
	2. Notify of layout changes
	3. Perform post-layout callbacks
	4. Redraw controls as needed
	*/

	// Perform layout calculations
	/** Layout process for all controls
	1. Propagate maximum size updates down the tree
	2. Propagate minimum size updates up the tree

	Unconstrained pass 1 - skipped if height of top-most element is known
	3. Measure desired widths up the tree
	4. Propagate proposed width down the tree + Measure desired height up the tree

	Height constrained pass 2 - height updates cannot decrease the final value
	5. Propagate proposed height down the tree + Measure desired width up the tree
	6. Propagate proposed width down the tree + Measure desired height up the tree

	7. Allocate down the tree
	**/

	print_line_debug("");
	print_line_debug("Starting main layout loop");
	is_doing_layout = true;

	// Reset change flags for all controls in dirty buckets before processing
	for (int64_t depth = 0; depth <= max_dirty_depth; depth++) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		for (uint32_t i = 0; i < bucket->dirty_layouts.size(); i++) {
			RID rid = bucket->dirty_layouts[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (cache) {
				cache->size_changed = false;
				cache->position_changed = false;
			}
		}
	}

	// 1. Propagate maximum size updates down the tree
	// print_line_debug("LayoutServer::layout() - Propagating maximum size updates down the tree.");
	// print_line_debug("LayoutServer::layout() - Maximum dirty depth: " + itos(max_dirty_depth));
	for (int64_t depth = 0; depth <= max_dirty_depth; depth++) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		// print_line_debug("LayoutServer::layout() - Processing " + itos(bucket->max_sizes.size()) + " controls with maximum size updates at depth: " + itos(depth));
		for (uint32_t i = 0; i < bucket->dirty_max_sizes.size(); i++) {
			RID rid = bucket->dirty_max_sizes[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				// print_line_debug("LayoutServer::layout() - Control not found for RID: " + itos(rid.get_id()) + " at depth: " + itos(depth));
				continue;
			}

			bool updated = _update_max_size_cache(rid);
			if (!updated && !cache->dirty_flags.has_flag(DIRTY_MAX_SIZE_PROPAGATION)) {
				continue;
			}
			_update_layout(rid);

			Size2 child_max_size = cache->propagate_maximum_size ? cache->inner_combined_max_size : INVALID_SIZE;
			for (RID child : cache->children) {
				ControlLayoutCache *child_cache = cache_owner.get_or_null(child);
				ERR_CONTINUE_MSG(!child_cache, "Control not found for RID: " + String::num(child.get_id()) + ".");
				if (child_cache->parent_maximum_size_cache == child_max_size) {
					continue;
				}
				child_cache->parent_maximum_size_cache = child_max_size;
				_update_maximum_size(child);
			}

			cache->dirty_flags.clear_flag(DIRTY_MAX_SIZE_PROPAGATION);
		}
	}

	// 2. Propagate minimum size updates up the tree
	// print_line_debug("LayoutServer::layout() - Propagating minimum size updates up the tree.");
	// print_line_debug("LayoutServer::layout() - Maximum dirty depth: ", max_dirty_depth);
	for (int64_t depth = max_dirty_depth; depth >= 0; depth--) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		// print_line_debug("LayoutServer::layout() - Processing ", bucket->min_sizes.size(), " controls with minimum size updates at depth: ", depth);
		for (uint32_t i = 0; i < bucket->dirty_min_sizes.size(); i++) {
			RID rid = bucket->dirty_min_sizes[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				continue;
			}

			bool updated = _update_min_size_cache(rid);
			if (!updated) {
				continue;
			}
			_update_layout(rid);

			if (cache->parent != RID() && cache->layout_mode == LAYOUT_MODE_CONTAINER) {
				_update_minimum_size(cache->parent);
				_update_layout(cache->parent);
			}
		}
	}

	// Unconstrained pass 1
	// 3. Measure desired widths up the tree (doubles as invalidation propagation step)
	_desired_sizes_pass(Axis::AXIS_X);

	// 4. Propagate proposed width down the tree + Measure desired height up the tree
	_layout_pass(Axis::AXIS_X);

	// Height constrained pass 2 - height updates cannot decrease the final value
	// 5. Propagate proposed height down the tree + Measure desired width up the tree
	_layout_pass(Axis::AXIS_Y);

	// 6. Propagate proposed width down the tree + Measure desired height up the tree
	_layout_pass(Axis::AXIS_X, true);

	// 7. Allocate down the tree
	for (int64_t depth = 0; depth <= max_dirty_depth; depth++) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		for (uint32_t i = 0; i < bucket->dirty_layouts.size(); i++) {
			RID rid = bucket->dirty_layouts[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				continue;
			}

			cache->size_changed = !get_size(rid).is_equal_approx(get_proposed_size(rid));
			cache->position_changed = !get_position(rid).is_equal_approx(get_proposed_position(rid));

			if (cache->size_changed || cache->position_changed) {
				notification_queue.push_back(rid);
			}

			cache->offsets = cache->proposed_offsets;
			cache->anchors = cache->proposed_anchors;

			// BACKWARDS COMPAT CODE
			proposing_rid = rid;
			if (Container *container = static_cast<Container *>(cache->control)) {
				container->_sort_children();
			}
			proposing_rid = RID();
			// END OF BACKWARDS COMPAT CODE
		}
	}

	// Notify updates down the tree
	print_line_debug("LayoutServer::layout() - Notifying updates the tree.");
	for (uint32_t i = 0; i < redraw_queue.size(); i++) {
		RID rid = redraw_queue[i];
		ControlLayoutCache *cache = cache_owner.get_or_null(rid);
		if (!cache) {
			continue;
		}
		Control *control = cache->control;

		// print_line_debug("LayoutServer::layout() - Notifying control with RID: ", rid.get_id(), " of layout changes. Size changed: ", cache->size_changed, ", Position changed: ", cache->position_changed);

		if (control->is_inside_tree()) {
			if (cache->size_changed || cache->position_changed) {
				control->_notify_transform();
				control->emit_signal(SceneStringName(item_rect_changed));
				if (cache->size_changed) {
					control->notification(Control::NOTIFICATION_RESIZED);
					queue_redraw(rid);
				}
			}
			if (cache->position_changed && !cache->size_changed) {
				control->_update_canvas_item_transform();
			}
			control->queue_accessibility_update();
		} else if (cache->position_changed) {
			control->_notify_transform();
		}

		cache->size_changed = false;
		cache->position_changed = false;
	}

	_clear_layout_buckets();
	max_dirty_depth = 0;
	is_doing_layout = false;

	// Post-layout callbacks
	// print_line_debug("LayoutServer::layout() - Performing post-layout callbacks: " + itos(post_layout_callbacks.size()));
	for (uint32_t i = 0; i < callback_queue.size(); i++) {
		callback_queue[i].call();
	}
	callback_queue.clear();

	// Redraw controls
	print_line_debug("LayoutServer::layout() - Redrawing controls: " + itos(redraw_queue.size()));
	for (uint32_t i = 0; i < redraw_queue.size(); i++) {
		RID rid = redraw_queue[i];
		ControlLayoutCache *cache = cache_owner.get_or_null(rid);
		ERR_CONTINUE_MSG(!cache, "ControlLayoutCache not found for RID: " + String::num(rid.get_id()) + ".");
		Control *control = cache->control;
		cache->dirty_flags.clear_flag(DIRTY_DRAW);
		control->_redraw_callback();
	}
	redraw_queue.clear();

	print_line_debug("Finished main layout loop");
	print_line_debug("");
}

/// Instance management.

void LayoutServer::_clear_layout_buckets() {
	for (KeyValue<uint64_t, LayoutBucket *> &E : dirty_buckets) {
		memdelete(E.value);
	}
	dirty_buckets.clear();
}

LayoutServer::LayoutServer() {
	ERR_FAIL_COND_MSG(singleton, "LayoutServer singleton is already initialized.");
	singleton = this;
}

LayoutServer::~LayoutServer() {
	_clear_layout_buckets();
	singleton = nullptr;
}

/// Debug

void LayoutServer::mute(bool p_mute) {
	if (muted == p_mute) {
		return;
	}
	muted = p_mute;
	print_line("LayoutServer::mute() - Muted: ", muted);
}

template <typename... Args>
void LayoutServer::print_line_debug(Args... args) const {
	if (muted) {
		return;
	}
	print_line(args...);
}
