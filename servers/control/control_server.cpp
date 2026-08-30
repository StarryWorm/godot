/**************************************************************************/
/*  control_server.cpp                                                    */
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

#include "control_server.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "scene/gui/container.h"
#include "scene/gui/control.h"
#include "scene/scene_string_names.h"

ControlServer *ControlServer::singleton = nullptr;

#define FETCH_CACHE(rid) \
	ERR_FAIL_COND_MSG(!cache_owner.owns(rid), "ControlLayoutCache does not own the RID: " + itos(rid.get_id())); \
	ControlLayoutCache *cache = cache_owner.get_or_null(rid); \
	ERR_FAIL_NULL_MSG(cache, "ControlLayoutCache not found for RID: " + itos(rid.get_id()));

#define FETCH_CACHE_V(rid, ret) \
	ERR_FAIL_COND_V_MSG(!cache_owner.owns(rid), ret, "ControlLayoutCache does not own the RID: " + itos(rid.get_id())); \
	ControlLayoutCache *cache = cache_owner.get_or_null(rid); \
	ERR_FAIL_NULL_V_MSG(cache, ret, "ControlLayoutCache not found for RID: " + itos(rid.get_id()));

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

RID ControlServer::add_control(Control *p_control) {
	ERR_FAIL_COND_V_MSG(is_doing_layout, RID(), "Attempting to modify control tree (adding new control) while doing layout. Use call_deferred instead.");
	RID control_rid = cache_owner.make_rid();

	ControlLayoutCache *cache = cache_owner.get_or_null(control_rid);
	ERR_FAIL_NULL_V_MSG(cache, RID(), "ControlLayoutCache allocation failed.");

	print_line_debug("Adding Control ", control_rid.get_id());

	managed_controls[control_rid] = p_control;
	return control_rid;
}

void ControlServer::free_control(RID p_rid) {
	ERR_FAIL_COND_MSG(is_doing_layout, "Attempting to modify control tree (freeing control) while doing layout. Use call_deferred instead.");
	FETCH_CACHE(p_rid);

	ControlLayoutCache *parent_cache = cache_owner.get_or_null(cache->parent);
	if (parent_cache) {
		parent_cache->children.erase(p_rid);
		if (cache->layout_mode == LAYOUT_MODE_CONTAINER) {
			update_minimum_size(cache->parent);
			update_desired_size(cache->parent);
			update_layout(cache->parent);
		}
	}

	int64_t depth = cache->depth;
	if (dirty_buckets.has(depth)) {
		dirty_buckets[depth]->erase(p_rid);
	}

	queued_for_redraw.erase(p_rid);
	managed_controls.erase(p_rid);
	cache_owner.free(p_rid);
}

/// Tree management.

void ControlServer::set_control_parent(RID p_rid, RID p_parent) {
	ERR_FAIL_COND_MSG(is_doing_layout, "Attempting to modify control tree (setting parent) while doing layout. Use call_deferred instead.");
	FETCH_CACHE(p_rid);

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

void ControlServer::set_control_depth(RID p_rid, int p_depth) {
	ERR_FAIL_COND_MSG(is_doing_layout, "Attempting to modify control tree (setting depth) while doing layout. Use call_deferred instead.");
	FETCH_CACHE(p_rid);

	if ((int)cache->depth == p_depth) {
		return;
	}

	if (dirty_buckets.has(cache->depth)) {
		dirty_buckets[cache->depth]->erase(p_rid);
	}

	FETCH_BUCKET(p_depth);
	UPDATE_DIRTY_DEPTH(p_depth);

	if (cache->min_size_dirty) {
		bucket->dirty_min_sizes.push_back(p_rid);
	}
	if (cache->max_size_dirty) {
		bucket->dirty_max_sizes.push_back(p_rid);
	}
	if (cache->desired_size_dirty) {
		bucket->dirty_desired_sizes.push_back(p_rid);
	}
	if (cache->layout_dirty) {
		bucket->dirty_layouts.push_back(p_rid);
	}
	if (cache->state_changed) {
		bucket->changed_states.push_back(p_rid);
	}

	cache->depth = p_depth;
	for (RID child : cache->children) {
		set_control_depth(child, p_depth + 1);
	}
}

void ControlServer::set_control_as_internal_child(RID p_rid, bool p_internal) {
	FETCH_CACHE(p_rid);
	cache->is_internal_child = p_internal;
}

/// State management.

void ControlServer::update_visibility(RID p_rid) {
	if (p_rid.is_null()) {
		return;
	}

	FETCH_CACHE(p_rid);

	if (cache->parent == RID() || cache->layout_mode != LAYOUT_MODE_CONTAINER || cache->is_internal_child) {
		return;
	}

	if (is_doing_layout) {
		WARN_PRINT("Attempting to update visibility for a contained child while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &ControlServer::update_visibility).call_deferred(p_rid);
		return;
	}

	update_minimum_size(cache->parent);
	update_desired_size(cache->parent);
	update_layout(cache->parent);
}

void ControlServer::update_layout(RID p_rid) {
	if (is_doing_layout) {
		WARN_PRINT("Attempting to update layout while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &ControlServer::update_layout).call_deferred(p_rid);
		return;
	}
	_update_layout(p_rid);
}

void ControlServer::_update_layout(RID p_rid) {
	FETCH_CACHE(p_rid);

	if (cache->layout_dirty) {
		return;
	}

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->dirty_layouts.push_back(p_rid);
	cache->layout_dirty = true;
}

void ControlServer::update_parent_layout(RID p_rid) {
	if (p_rid.is_null()) {
		return;
	}

	FETCH_CACHE(p_rid);

	if (cache->parent.is_null() || cache->layout_mode != LAYOUT_MODE_CONTAINER) {
		return;
	}

	update_layout(cache->parent);
}

void ControlServer::update_minimum_size(RID p_rid) {
	if (is_doing_layout) {
		WARN_PRINT("Attempting to update minimum size while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &ControlServer::update_minimum_size).call_deferred(p_rid);
		return;
	}
	_update_minimum_size(p_rid);
}

void ControlServer::_update_minimum_size(RID p_rid) {
	NULL_RID(p_rid);
	FETCH_CACHE(p_rid);

	if (cache->min_size_dirty) {
		return;
	}

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->dirty_min_sizes.push_back(p_rid);
	cache->min_size_dirty = true;
}

bool ControlServer::_update_min_size_cache(RID p_rid) {
	FETCH_CACHE_V(p_rid, false);
	ERR_FAIL_COND_V_MSG(!managed_controls.has(p_rid), false, "Control is missing for rid " + String::num(p_rid.get_id()) + ".");

	Control *control = managed_controls.get(p_rid);
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
	cache->min_size_dirty = false;
	control->emit_signal(SceneStringName(minimum_size_changed));
	return updated;
}

void ControlServer::update_maximum_size(RID p_rid) {
	if (is_doing_layout) {
		WARN_PRINT("Attempting to update maximum size while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &ControlServer::update_maximum_size).call_deferred(p_rid);
		return;
	};
	_update_maximum_size(p_rid);
}

void ControlServer::_update_maximum_size(RID p_rid) {
	NULL_RID(p_rid);
	FETCH_CACHE(p_rid);

	if (cache->max_size_dirty) {
		return;
	}

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->dirty_max_sizes.push_back(p_rid);
	cache->max_size_dirty = true;
}

bool ControlServer::_update_max_size_cache(RID p_rid) {
	FETCH_CACHE_V(p_rid, false);
	ERR_FAIL_COND_V_MSG(!managed_controls.has(p_rid), false, "Control is missing for rid " + String::num(p_rid.get_id()) + ".");

	Control *control = managed_controls.get(p_rid);
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
	cache->max_size_dirty = false;
	control->emit_signal(SceneStringName(maximum_size_changed));
	return updated;
}

void ControlServer::update_desired_size(RID p_rid) {
	if (is_doing_layout) {
		WARN_PRINT("Attempting to update desired size while doing layout. Use call_deferred instead. Automatically deferring call.");
		callable_mp(this, &ControlServer::update_desired_size).call_deferred(p_rid);
		return;
	};
	_update_desired_size(p_rid);
}

void ControlServer::_update_desired_size(RID p_rid) {
	NULL_RID(p_rid);
	FETCH_CACHE(p_rid);

	if (cache->desired_size_dirty) {
		return;
	}

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->dirty_desired_sizes.push_back(p_rid);
	cache->desired_size_dirty = true;
}

bool ControlServer::_update_preferred_width_cache(RID p_rid) {
	FETCH_CACHE_V(p_rid, false);
	ERR_FAIL_COND_V_MSG(!managed_controls.has(p_rid), false, "Control is missing for rid " + String::num(p_rid.get_id()) + ".");

	Control *control = managed_controls.get(p_rid);
	if (!control->is_inside_tree()) {
		return false;
	}

	real_t preferred_width = control->get_preferred_width();
	// print_line_debug("Control: ", p_rid.get_id(), " preferred width updated to: ", preferred_width, " (was: ", cache->desired_size.width, ")");
	bool updated = cache->desired_size.width != preferred_width;
	cache->desired_size.width = preferred_width;

	return updated;
}

bool ControlServer::_update_desired_height_cache(RID p_rid) {
	FETCH_CACHE_V(p_rid, false);
	ERR_FAIL_COND_V_MSG(!managed_controls.has(p_rid), false, "Control is missing for rid " + String::num(p_rid.get_id()) + ".");

	Control *control = managed_controls.get(p_rid);
	if (!control->is_inside_tree()) {
		return false;
	}

	real_t desired_height = control->get_desired_height();
	// print_line_debug("Control: ", p_rid.get_id(), " desired height updated to: ", desired_height, " (was: ", cache->desired_size.height, ")");
	bool updated = cache->desired_size.height != desired_height;
	cache->desired_size.height = desired_height;

	cache->desired_size_dirty = false;
	return updated;
}

void ControlServer::_update_state(RID p_rid) {
	NULL_RID(p_rid);
	FETCH_CACHE(p_rid);

	if (cache->state_changed) {
		return;
	}

	FETCH_BUCKET(cache->depth);
	UPDATE_DIRTY_DEPTH(cache->depth);
	bucket->changed_states.push_back(p_rid);
	cache->state_changed = true;
}

void ControlServer::queue_redraw(RID p_rid) {
	FETCH_CACHE(p_rid);
	if (cache->draw_dirty) {
		return;
	}
	cache->draw_dirty = true;
	queued_for_redraw.push_back(p_rid);
}

/// Internal Position and Sizing methods

// FIXME: Figure out if we can't fold this method into `Control`.
// Currently used by some `CanvasItem`s that could realistically be better off as `Control`? And deprecate the others since they don't make sense for UI positioning.
// Full list (July 2026): Sprite2D, AnimatedSprite2D, PointLight2D (???), TouchScreenButton (docs say this one doesn't even support...), BackBufferCopy (docs say this one doesn't even support...)
Rect2 ControlServer::_get_parent_anchorable_rect(RID p_rid) const {
	ERR_FAIL_COND_V_MSG(!managed_controls.has(p_rid), Rect2(), "Control is missing for rid " + String::num(p_rid.get_id()) + ".");
	Control *control = managed_controls.get(p_rid);
	return control->get_parent_anchorable_rect();
}

void ControlServer::_fit_uncontained_control(RID p_rid, Axis p_axis) {
	// Resolution order, depending on what is dirty:
	// A. Anchors then offsets
	// B. Size then position
	// Branch B always gets called, as it handles minimum/maximum size clamping.
	FETCH_CACHE(p_rid);
	ERR_FAIL_COND_MSG(!managed_controls.has(p_rid), "Control is missing for rid " + String::num(p_rid.get_id()) + ".");

	print_line_debug("Fitting uncontained control: ", p_rid.get_id(), " on axis: ", p_axis);

	real_t old_size = get_size(p_rid)[p_axis];
	real_t old_position = get_position(p_rid)[p_axis];
	print_line_debug("Old size: ", old_size, ", Old position: ", old_position);

	Side side = (p_axis == AXIS_HORIZONTAL) ? SIDE_LEFT : SIDE_TOP;
	Side opposite_side = (p_axis == AXIS_HORIZONTAL) ? SIDE_RIGHT : SIDE_BOTTOM;

	if (cache->anchors[side] != cache->new_anchors[side] || cache->offsets[side] != cache->new_offsets[side] ||
			cache->anchors[opposite_side] != cache->new_anchors[opposite_side] || cache->offsets[opposite_side] != cache->new_offsets[opposite_side]) {
		_fit_anchors(p_rid, p_axis);
		bool explicit_size = cache->explicit_size;
		cache->explicit_size = true;
		print_line_debug("After anchors, new anchors: ", cache->anchors, ", new offsets: ", cache->offsets);
		_fit_control_rect(p_rid, p_axis);
		print_line_debug("After rect fit, new anchors: ", cache->anchors, ", new offsets: ", cache->offsets);
		cache->explicit_size = explicit_size;
	} else {
		_fit_control_rect(p_rid, p_axis);
		print_line_debug("After rect fit, new anchors: ", cache->anchors, ", new offsets: ", cache->offsets);
	}

	real_t new_size = get_size(p_rid)[p_axis];
	real_t new_position = get_position(p_rid)[p_axis];
	bool size_changed = !Math::is_equal_approx(old_size, new_size);
	bool position_changed = !Math::is_equal_approx(old_position, new_position);
	print_line_debug("Size changed: ", size_changed, ", Position changed: ", position_changed);
	cache->size_changed = size_changed || cache->size_changed;
	cache->position_changed = position_changed || cache->position_changed;
	_sync_new_anchors_and_offsets(p_rid, p_axis);

	if (size_changed || position_changed) {
		_update_state(p_rid);
	}
}

void ControlServer::_fit_anchors(RID p_rid, Axis p_axis) {
	FETCH_CACHE(p_rid);

	Side side = (p_axis == AXIS_HORIZONTAL) ? SIDE_LEFT : SIDE_TOP;
	Side opposite_side = (p_axis == AXIS_HORIZONTAL) ? SIDE_RIGHT : SIDE_BOTTOM;

	_set_anchor(p_rid, side);
	_set_anchor(p_rid, opposite_side);
	cache->offsets[side] = cache->new_offsets[side];
	cache->offsets[opposite_side] = cache->new_offsets[opposite_side];

	real_t parent_size = _get_parent_anchorable_rect(p_rid).size[p_axis];
	cache->new_size[p_axis] = parent_size * (cache->anchors[opposite_side] - cache->anchors[side]) + cache->offsets[opposite_side] - cache->offsets[side];
	cache->new_position[p_axis] = cache->anchors[side] * parent_size + cache->offsets[side];
}

void ControlServer::_set_anchor(RID p_rid, Side p_side) {
	FETCH_CACHE(p_rid);

	Rect2 parent_rect = _get_parent_anchorable_rect(p_rid);
	real_t parent_range = (p_side == SIDE_LEFT || p_side == SIDE_RIGHT) ? parent_rect.size.x : parent_rect.size.y;
	real_t previous_pos = cache->offsets[p_side] + cache->anchors[p_side] * parent_range;
	real_t previous_opposite_pos = cache->offsets[(p_side + 2) % 4] + cache->anchors[(p_side + 2) % 4] * parent_range;

	cache->anchors[p_side] = cache->new_anchors[p_side];

	if (((p_side == SIDE_LEFT || p_side == SIDE_TOP) && cache->anchors[p_side] > cache->anchors[(p_side + 2) % 4]) ||
			((p_side == SIDE_RIGHT || p_side == SIDE_BOTTOM) && cache->anchors[p_side] < cache->anchors[(p_side + 2) % 4])) {
		if (cache->push_opposite_anchor[p_side]) {
			cache->anchors[(p_side + 2) % 4] = cache->anchors[p_side];
		} else {
			cache->anchors[p_side] = cache->anchors[(p_side + 2) % 4];
		}
	}

	if (!cache->anchor_keep_offsets[p_side]) {
		cache->offsets[p_side] = previous_pos - cache->anchors[p_side] * parent_range;
		if (cache->push_opposite_anchor[(p_side + 2) % 4]) {
			cache->offsets[(p_side + 2) % 4] = previous_opposite_pos - cache->anchors[(p_side + 2) % 4] * parent_range;
		}
	}
}

void ControlServer::_fit_control_rect(RID p_rid, Axis p_axis) {
	FETCH_CACHE(p_rid);
	Control *control = managed_controls.get(p_rid);

	real_t parent_rect_size = _get_parent_anchorable_rect(p_rid).size[p_axis];
	if (cache->new_rect_keep_offsets) {
		ERR_FAIL_COND_MSG(parent_rect_size <= 0, "Parent anchorable rect size is invalid.");
	}

	Size2 base_new_size = cache->explicit_size ? cache->new_size : cache->last_user_defined_size;
	real_t new_size = base_new_size.max(cache->combined_min_size)[p_axis];
	if (cache->combined_max_size[p_axis] >= 0) {
		new_size = MAX(new_size, cache->desired_size[p_axis]);
		new_size = MIN(new_size, cache->combined_max_size[p_axis]);
	}
	real_t size_delta = new_size - get_size(p_rid)[p_axis];
	real_t new_position = cache->explicit_position ? cache->new_position[p_axis] : get_position(p_rid)[p_axis];
	real_t pos = get_position(p_rid)[p_axis];

	print_line_debug("Fitting control rect for RID: ", p_rid.get_id(), " on axis: ", p_axis, " new size: ", new_size, " size delta: ", size_delta, " old position: ", pos, "new position: ", new_position);
	print_line_debug("Size info: min: ", cache->combined_min_size[p_axis], " max: ", cache->combined_max_size[p_axis], " desired: ", cache->desired_size[p_axis], " explicit: ", cache->explicit_size, " last user defined: ", cache->last_user_defined_size[p_axis]);

	// Only compute size-driven position changes if position is not being explicitly updated.
	if (new_position == pos && size_delta != 0) {
		int rtl_multiplier = (p_axis == AXIS_HORIZONTAL && control->is_layout_rtl()) ? -1 : 1;
		GrowDirection grow_direction = (p_axis == AXIS_HORIZONTAL) ? cache->h_grow : cache->v_grow;
		float grow_factor =
				(grow_direction == GROW_DIRECTION_BEGIN) ? 1.0f
				: (grow_direction == GROW_DIRECTION_END) ? 0.0f
														 : 0.5f;

		new_position -= size_delta * rtl_multiplier * grow_factor;
	}

	Side side = (p_axis == AXIS_HORIZONTAL) ? SIDE_LEFT : SIDE_TOP;
	Side opposite_side = (p_axis == AXIS_HORIZONTAL) ? SIDE_RIGHT : SIDE_BOTTOM;

	if (new_position != pos || size_delta != 0) {
		if (cache->new_rect_keep_offsets) {
			cache->anchors[side] = (new_position - cache->offsets[side]) / parent_rect_size;
			cache->anchors[opposite_side] = (new_position + new_size - cache->offsets[opposite_side]) / parent_rect_size;
		} else {
			cache->offsets[side] = new_position - (cache->anchors[side] * parent_rect_size);
			cache->offsets[opposite_side] = new_position + new_size - (cache->anchors[opposite_side] * parent_rect_size);
		}
	}
}

void ControlServer::_fit_contained_control(RID p_rid, Axis p_axis) {
	FETCH_CACHE(p_rid);

	Side side = (p_axis == AXIS_HORIZONTAL) ? SIDE_LEFT : SIDE_TOP;
	Side opposite_side = (p_axis == AXIS_HORIZONTAL) ? SIDE_RIGHT : SIDE_BOTTOM;

	_sync_anchor(p_rid, side);
	_sync_anchor(p_rid, opposite_side);
	if (cache->explicit_size || cache->explicit_position) {
		_fit_control_rect(p_rid, p_axis);
		_sync_new_anchors_and_offsets(p_rid, p_axis);
	}
}

void ControlServer::_sync_anchor(RID p_rid, Side p_side) {
	FETCH_CACHE(p_rid);

	if (cache->anchors[p_side] != cache->new_anchors[p_side] || cache->offsets[p_side] != cache->new_offsets[p_side]) {
		cache->anchors[p_side] = cache->new_anchors[p_side];
		cache->offsets[p_side] = cache->new_offsets[p_side];
		_update_layout(p_rid);
	}
}

void ControlServer::_layout_contained_control(RID p_rid, Axis p_axis, real_t p_pos, real_t p_size) {
	FETCH_CACHE(p_rid);

	real_t old_pos = get_position(p_rid)[p_axis];
	real_t old_size = get_size(p_rid)[p_axis];

	real_t normalized_size = MAX(p_size, cache->combined_min_size[p_axis]);
	if (cache->combined_max_size[p_axis] >= 0) {
		normalized_size = MIN(normalized_size, cache->combined_max_size[p_axis]);
	}

	bool pos_changed = !Math::is_equal_approx(old_pos, p_pos);
	bool size_changed = !Math::is_equal_approx(old_size, normalized_size);

	if (!pos_changed && !size_changed) {
		return;
	}

	Side side = (p_axis == AXIS_HORIZONTAL) ? SIDE_LEFT : SIDE_TOP;
	Side opposite_side = (p_axis == AXIS_HORIZONTAL) ? SIDE_RIGHT : SIDE_BOTTOM;
	cache->anchors[side] = 0;
	cache->anchors[opposite_side] = 0;
	cache->offsets[side] = p_pos;
	cache->offsets[opposite_side] = p_pos + normalized_size;
	_sync_new_anchors_and_offsets(p_rid, p_axis);

	if (size_changed) {
		_update_layout(p_rid);
	}

	cache->size_changed = size_changed || cache->size_changed;
	cache->position_changed = pos_changed || cache->position_changed;

	_update_state(p_rid);
}

void ControlServer::_sync_new_anchors_and_offsets(RID p_rid, Axis p_axis) {
	FETCH_CACHE(p_rid);

	Side side = (p_axis == AXIS_HORIZONTAL) ? SIDE_LEFT : SIDE_TOP;
	Side opposite_side = (p_axis == AXIS_HORIZONTAL) ? SIDE_RIGHT : SIDE_BOTTOM;

	cache->new_anchors[side] = cache->anchors[side];
	cache->new_anchors[opposite_side] = cache->anchors[opposite_side];
	cache->new_offsets[side] = cache->offsets[side];
	cache->new_offsets[opposite_side] = cache->offsets[opposite_side];
}

/// Positioning and Sizing API

void ControlServer::set_default_children_layout_mode(RID p_rid, LayoutMode p_mode) {
	FETCH_CACHE(p_rid);
	cache->default_children_layout_mode = p_mode;
}

CS::LayoutMode ControlServer::get_default_children_layout_mode(RID p_rid) const {
	FETCH_CACHE_V(p_rid, LAYOUT_MODE_POSITION);
	return cache->default_children_layout_mode;
}

void ControlServer::set_layout_mode(RID p_rid, LayoutMode p_mode) {
	FETCH_CACHE(p_rid);
	cache->layout_mode = p_mode;
}

CS::LayoutMode ControlServer::get_layout_mode(RID p_rid) const {
	FETCH_CACHE_V(p_rid, LAYOUT_MODE_POSITION);
	return cache->layout_mode;
}

CS::LayoutMode ControlServer::get_default_layout_mode(RID p_rid) const {
	FETCH_CACHE_V(p_rid, LAYOUT_MODE_POSITION);
	ControlLayoutCache *parent = cache_owner.get_or_null(cache->parent);
	return parent ? parent->default_children_layout_mode : LAYOUT_MODE_POSITION;
}

void ControlServer::set_h_grow_direction(RID p_rid, CS::GrowDirection p_direction) {
	FETCH_CACHE(p_rid);
	ERR_FAIL_INDEX((int)p_direction, 3);
	cache->h_grow = p_direction;
}

ControlServer::GrowDirection ControlServer::get_h_grow_direction(RID p_rid) const {
	FETCH_CACHE_V(p_rid, GROW_DIRECTION_BEGIN);
	return cache->h_grow;
}

void ControlServer::set_v_grow_direction(RID p_rid, CS::GrowDirection p_direction) {
	FETCH_CACHE(p_rid);
	ERR_FAIL_INDEX((int)p_direction, 3);
	cache->v_grow = p_direction;
}

ControlServer::GrowDirection ControlServer::get_v_grow_direction(RID p_rid) const {
	FETCH_CACHE_V(p_rid, GROW_DIRECTION_BEGIN);
	return cache->v_grow;
}

Vector4 ControlServer::get_anchors(RID p_rid) const {
	FETCH_CACHE_V(p_rid, Vector4());
	return cache->anchors;
}

void ControlServer::set_anchor(RID p_rid, Side p_side, real_t p_anchor, bool p_keep_offset, bool p_push_opposite_anchor) {
	ERR_FAIL_INDEX((int)p_side, 4);
	FETCH_CACHE(p_rid);

	if (p_anchor == cache->anchors[p_side]) {
		return;
	}

	cache->new_anchors[p_side] = p_anchor;
	cache->anchor_keep_offsets[p_side] = p_keep_offset;
	cache->push_opposite_anchor[p_side] = p_push_opposite_anchor;
	// print_line_debug("Setting anchor for control: " + itos(p_rid.get_id()) + " side: " + itos(p_side) + " to: " + rtos(p_anchor) + ", keep_offset: " + itos(p_keep_offset) + ", push_opposite: " + itos(p_push_opposite_anchor));
	_update_layout(p_rid);
}

Vector4 ControlServer::get_offsets(RID p_rid) const {
	FETCH_CACHE_V(p_rid, Vector4());
	return cache->offsets;
}

void ControlServer::set_offset(RID p_rid, Side p_side, real_t p_value) {
	ERR_FAIL_COND(!std::isfinite(p_value));
	FETCH_CACHE(p_rid);

	if (p_value == cache->offsets[p_side]) {
		return;
	}

	cache->new_offsets[p_side] = p_value;
	_update_layout(p_rid);
}

Size2 ControlServer::get_size(RID p_rid) const {
	FETCH_CACHE_V(p_rid, INVALID_SIZE);
	Size2 parent_rect = _get_parent_anchorable_rect(p_rid).size;
	real_t width = parent_rect.width * (cache->anchors[SIDE_RIGHT] - cache->anchors[SIDE_LEFT]) + cache->offsets[SIDE_RIGHT] - cache->offsets[SIDE_LEFT];
	real_t height = parent_rect.height * (cache->anchors[SIDE_BOTTOM] - cache->anchors[SIDE_TOP]) + cache->offsets[SIDE_BOTTOM] - cache->offsets[SIDE_TOP];
	return Size2(width, height);
}

void ControlServer::set_size(RID p_rid, Size2 p_size, bool p_keep_offsets) {
	FETCH_CACHE(p_rid);
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

	if (get_size(p_rid) == new_size) {
		return;
	}

	cache->last_user_defined_size = new_size;
	cache->new_size = new_size;
	cache->new_rect_keep_offsets = p_keep_offsets;
	cache->explicit_size = true;
	_update_layout(p_rid);

	// Notes: Currently, the user needs to make sure the minimum size is up to date before calling this, which may lead to some unexpected behavior in certain cases.
	// See test_tab_bar.cpp:"Ensure Tab Visible" for an example of how this can be a problem. The TabBar's set_clip_tabs needs to be processed before set_size is called.
	// TODO: Do the clamping after min/max updates & propagation.
}

Point2 ControlServer::get_position(RID p_rid) const {
	FETCH_CACHE_V(p_rid, Point2());
	Rect2 parent_rect = _get_parent_anchorable_rect(p_rid);
	real_t h_pos = cache->anchors[SIDE_LEFT] * parent_rect.size.width + cache->offsets[SIDE_LEFT];
	real_t v_pos = cache->anchors[SIDE_TOP] * parent_rect.size.height + cache->offsets[SIDE_TOP];
	return Point2(h_pos, v_pos);
}

void ControlServer::set_position(RID p_rid, Point2 p_position, bool p_keep_offsets) {
	FETCH_CACHE(p_rid);
	// print_line_debug("Setting position for control: ", itos(p_rid.get_id()), " to: ", p_position);

	if (get_position(p_rid) == p_position) {
		return;
	}

	cache->new_position = p_position;
	cache->new_rect_keep_offsets = p_keep_offsets;
	cache->explicit_position = true;
	_update_layout(p_rid);
}

// TODO: Make sure the documentation reflects that this method IMMEDIATELY takes effect. Meant for layout purposes ONLY.
void ControlServer::set_horizontal_layout(RID p_rid, real_t p_pos, real_t p_width) {
	_layout_contained_control(p_rid, AXIS_HORIZONTAL, p_pos, p_width);
}

// TODO: Make sure the documentation reflects that this method IMMEDIATELY takes effect. Meant for layout purposes ONLY.
void ControlServer::set_vertical_layout(RID p_rid, real_t p_pos, real_t p_height) {
	_layout_contained_control(p_rid, AXIS_VERTICAL, p_pos, p_height);
}

Size2 ControlServer::get_custom_minimum_size(RID p_rid) const {
	FETCH_CACHE_V(p_rid, INVALID_SIZE);
	return cache->custom_min_size;
}

void ControlServer::set_custom_minimum_size(RID p_rid, Size2 p_size) {
	FETCH_CACHE(p_rid);
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

Size2 ControlServer::get_custom_maximum_size(RID p_rid) const {
	FETCH_CACHE_V(p_rid, INVALID_SIZE);
	return cache->custom_max_size;
}

void ControlServer::set_custom_maximum_size(RID p_rid, Size2 p_size) {
	FETCH_CACHE(p_rid);
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

Size2 ControlServer::get_combined_minimum_size(RID p_rid) const {
	FETCH_CACHE_V(p_rid, INVALID_SIZE);
	return cache->combined_min_size;
}

Size2 ControlServer::get_bound_minimum_size(RID p_rid) const {
	FETCH_CACHE_V(p_rid, INVALID_SIZE);
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

Size2 ControlServer::get_combined_maximum_size(RID p_rid) const {
	FETCH_CACHE_V(p_rid, INVALID_SIZE);
	return cache->combined_max_size;
}

bool ControlServer::is_propagating_maximum_size(RID p_rid) const {
	FETCH_CACHE_V(p_rid, false);
	return cache->propagate_maximum_size;
}

void ControlServer::set_propagate_maximum_size(RID p_rid, bool p_propagate) {
	FETCH_CACHE(p_rid);
	if (cache->propagate_maximum_size == p_propagate) {
		return;
	}
	cache->propagate_maximum_size = p_propagate;
	cache->propagate_maximum_size_dirty = true;
	update_maximum_size(p_rid);
}

void ControlServer::set_parent_maximum_size_cache(RID p_rid, Size2 p_size) {
	FETCH_CACHE(p_rid);
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

Size2 ControlServer::get_desired_size(RID p_rid) const {
	FETCH_CACHE_V(p_rid, Size2());
	return cache->desired_size;
}

bool ControlServer::is_layout_dirty(RID p_rid) const {
	FETCH_CACHE_V(p_rid, false);
	return cache->layout_dirty;
}

bool ControlServer::is_layout_dirty_in_tree(RID p_rid) const {
	FETCH_CACHE_V(p_rid, false);
	return cache->layout_dirty || is_layout_dirty_in_tree(cache->parent);
}

void ControlServer::add_post_layout_callback(const Callable &p_callable) {
	if (!p_callable.is_valid()) {
		return;
	}
	post_layout_callbacks.push_back(p_callable);
}

/// Main layout loop.

void ControlServer::layout() {
	// Method called by Main which synchronizes the control layout.
	/** Method responsibilities:
	 * 1. Perform layout calculations as needed
	 * 2. Perform post-layout callbacks
	 * 3. Redraw controls as needed
	 */

	// Perform layout calculations
	/** Layout process for all controls
	 * 1. Propagate maximum size updates down the tree
	 * 2. Propagate minimum size updates up the tree
	 * 3. Propagate preferred width updates up the tree
	 * 4. Perform width-based layout pass down the tree
	 * 5. Propagate desired height updates up the tree
	 * 6. Perform height-based layout pass down the tree
	 * 7. Notify updates down the tree
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
	// print_line_debug("ControlServer::layout() - Propagating maximum size updates down the tree.");
	// print_line_debug("ControlServer::layout() - Maximum dirty depth: " + itos(max_dirty_depth));
	for (int64_t depth = 0; depth <= max_dirty_depth; depth++) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		// print_line_debug("ControlServer::layout() - Processing " + itos(bucket->max_sizes.size()) + " controls with maximum size updates at depth: " + itos(depth));
		for (uint32_t i = 0; i < bucket->dirty_max_sizes.size(); i++) {
			RID rid = bucket->dirty_max_sizes[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				// print_line_debug("ControlServer::layout() - Control not found for RID: " + itos(rid.get_id()) + " at depth: " + itos(depth));
				continue;
			}

			bool updated = _update_max_size_cache(rid);
			if (!updated && !cache->propagate_maximum_size_dirty) {
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

			cache->propagate_maximum_size_dirty = false;
		}
	}

	// 2. Propagate minimum size updates up the tree
	// print_line_debug("ControlServer::layout() - Propagating minimum size updates up the tree.");
	// print_line_debug("ControlServer::layout() - Maximum dirty depth: ", max_dirty_depth);
	for (int64_t depth = max_dirty_depth; depth >= 0; depth--) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		// print_line_debug("ControlServer::layout() - Processing ", bucket->min_sizes.size(), " controls with minimum size updates at depth: ", depth);
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

	// 3. Propagate preferred width updates up the tree
	// print_line_debug("ControlServer::layout() - Propagating preferred width updates up the tree.");
	// print_line_debug("ControlServer::layout() - Maximum dirty depth: " + itos(max_dirty_depth));
	for (int64_t depth = max_dirty_depth; depth >= 0; depth--) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		// print_line_debug("ControlServer::layout() - Processing " + itos(bucket->desired_sizes.size()) + " controls with preferred width updates at depth: " + itos(depth));
		for (uint32_t i = 0; i < bucket->dirty_desired_sizes.size(); i++) {
			RID rid = bucket->dirty_desired_sizes[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				continue;
			}

			bool updated = _update_preferred_width_cache(rid);
			if (!updated) {
				continue;
			}
			_update_layout(rid);

			if (cache->parent != RID() && cache->layout_mode == LAYOUT_MODE_CONTAINER) {
				_update_desired_size(cache->parent);
				_update_layout(cache->parent);
			}
		}
	}

	// 4. Perform width-based layout pass down the tree
	print_line_debug("ControlServer::layout() - Performing width-based layout pass down the tree.");
	// print_line_debug("ControlServer::layout() - Maximum dirty depth: " + itos(max_dirty_depth));
	for (int64_t depth = 0; depth <= max_dirty_depth; depth++) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		print_line_debug("ControlServer::layout() - Processing ", bucket->dirty_layouts.size(), " controls with layout updates at depth: ", depth);
		for (uint32_t i = 0; i < bucket->dirty_layouts.size(); i++) {
			RID rid = bucket->dirty_layouts[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				continue;
			}

			if (cache->layout_mode != LAYOUT_MODE_CONTAINER) {
				print_line_debug("ControlServer::layout() - Fitting uncontained control with RID: ", rid.get_id(), " on horizontal axis.");
				_fit_uncontained_control(rid, AXIS_HORIZONTAL);
			} else {
				print_line_debug("ControlServer::layout() - Fitting contained control with RID: ", rid.get_id(), " on horizontal axis.");
				_fit_contained_control(rid, AXIS_HORIZONTAL);
			}

			ERR_CONTINUE_MSG(!managed_controls.has(rid), "Control not found for RID: " + String::num(rid.get_id()) + ".");
			Control *control = managed_controls.get(rid);
			print_line_debug("ControlServer::layout() - Fitting contained children for control with RID: ", rid.get_id(), " on horizontal axis.");
			control->_fit_contained_children(AXIS_HORIZONTAL);
		}
	}

	// 5. Propagate desired height updates up the tree
	// print_line_debug("ControlServer::layout() - Propagating desired height updates up the tree.");
	// print_line_debug("ControlServer::layout() - Maximum dirty depth: " + itos(max_dirty_depth));
	for (int64_t depth = max_dirty_depth; depth >= 0; depth--) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		// print_line_debug("ControlServer::layout() - Processing " + itos(bucket->desired_sizes.size()) + " controls with desired height updates at depth: " + itos(depth));
		for (uint32_t i = 0; i < bucket->dirty_desired_sizes.size(); i++) {
			RID rid = bucket->dirty_desired_sizes[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				continue;
			}

			bool updated = _update_desired_height_cache(rid);
			if (!updated) {
				continue;
			}

			if (cache->parent != RID() && cache->layout_mode == LAYOUT_MODE_CONTAINER) {
				_update_desired_size(cache->parent);
				_update_layout(cache->parent);
			}
		}
	}

	// 6. Perform height-based layout pass down the tree
	print_line_debug("ControlServer::layout() - Performing height-based layout pass down the tree.");
	// print_line_debug("ControlServer::layout() - Maximum dirty depth: " + itos(max_dirty_depth));
	for (int64_t depth = 0; depth <= max_dirty_depth; depth++) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		print_line_debug("ControlServer::layout() - Processing ", bucket->dirty_layouts.size(), " controls with layout updates at depth: ", depth);
		for (uint32_t i = 0; i < bucket->dirty_layouts.size(); i++) {
			RID rid = bucket->dirty_layouts[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				continue;
			}

			if (cache->layout_mode != LAYOUT_MODE_CONTAINER) {
				print_line_debug("ControlServer::layout() - Fitting uncontained control with RID: ", rid.get_id(), " on vertical axis.");
				_fit_uncontained_control(rid, AXIS_VERTICAL);
			} else {
				print_line_debug("ControlServer::layout() - Fitting contained control with RID: ", rid.get_id(), " on vertical axis.");
				_fit_contained_control(rid, AXIS_VERTICAL);
			}
			cache->explicit_position = false;
			cache->explicit_size = false;

			ERR_CONTINUE_MSG(!managed_controls.has(rid), "Control not found for RID: " + String::num(rid.get_id()) + ".");
			Control *control = managed_controls.get(rid);
			// Backwards compatibility, FIXME: remove when we break compatibility and remove the old Container notifications.
			Container *container = dynamic_cast<Container *>(control);
			if (container) {
				print_line_debug("ControlServer::layout() - Fitting contained children for container with RID: ", rid.get_id(), " with backwards compatibility.");
				container->_sort_children();
			}
			// End of backwards compatibility section.
			print_line_debug("ControlServer::layout() - Fitting contained children for control with RID: ", rid.get_id(), " on vertical axis.");
			control->_fit_contained_children(AXIS_VERTICAL);

			cache->layout_dirty = false;
		}
	}

	// 7. Notify updates down the tree
	print_line_debug("ControlServer::layout() - Notifying updates down the tree.");
	// print_line_debug("ControlServer::layout() - Maximum dirty depth: " + itos(max_dirty_depth));
	for (int64_t depth = 0; depth <= max_dirty_depth; depth++) {
		if (!dirty_buckets.has(depth)) {
			continue;
		}
		LayoutBucket *bucket = dirty_buckets[depth];
		print_line_debug("ControlServer::layout() - Processing " + itos(bucket->changed_states.size()) + " controls with changed layout at depth: " + itos(depth));
		for (uint32_t i = 0; i < bucket->changed_states.size(); i++) {
			RID rid = bucket->changed_states[i];
			ControlLayoutCache *cache = cache_owner.get_or_null(rid);
			if (!cache) {
				continue;
			}
			print_line_debug("ControlServer::layout() - Notifying control with RID: " + itos(rid.get_id()) + " of layout changes.");
			print_line_debug("ControlServer::layout() - Size changed: ", cache->size_changed, ", Position changed: ", cache->position_changed);

			ERR_CONTINUE_MSG(!managed_controls.has(rid), "Control not found for RID: " + String::num(rid.get_id()) + ".");
			Control *control = managed_controls.get(rid);

			// print_line_debug("ControlServer::layout() - Notifying control with RID: ", rid.get_id(), " of layout changes. Size changed: ", cache->size_changed, ", Position changed: ", cache->position_changed);

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
			cache->state_changed = false;
		}
	}

	_clear_layout_buckets();
	max_dirty_depth = 0;
	is_doing_layout = false;

	// Post-layout callbacks
	// print_line_debug("ControlServer::layout() - Performing post-layout callbacks: " + itos(post_layout_callbacks.size()));
	for (uint32_t i = 0; i < post_layout_callbacks.size(); i++) {
		post_layout_callbacks[i].call();
	}
	post_layout_callbacks.clear();

	// Redraw controls
	print_line_debug("ControlServer::layout() - Redrawing controls: " + itos(queued_for_redraw.size()));
	for (uint32_t i = 0; i < queued_for_redraw.size(); i++) {
		RID rid = queued_for_redraw[i];
		ControlLayoutCache *cache = cache_owner.get_or_null(rid);
		ERR_CONTINUE_MSG(!cache, "ControlLayoutCache not found for RID: " + String::num(rid.get_id()) + ".");
		ERR_CONTINUE_MSG(!managed_controls.has(rid), "Control not found for RID: " + String::num(rid.get_id()) + ".");
		Control *control = managed_controls.get(rid);
		cache->draw_dirty = false;
		control->_redraw_callback();
	}
	queued_for_redraw.clear();

	print_line_debug("Finished main layout loop");
	print_line_debug("");
}

/// Instance management.

void ControlServer::_clear_layout_buckets() {
	for (KeyValue<uint64_t, LayoutBucket *> &E : dirty_buckets) {
		memdelete(E.value);
	}
	dirty_buckets.clear();
}

ControlServer::ControlServer() {
	ERR_FAIL_COND_MSG(singleton, "ControlServer singleton is already initialized.");
	singleton = this;
}

ControlServer::~ControlServer() {
	_clear_layout_buckets();
	singleton = nullptr;
}

/// Debug

void ControlServer::mute(bool p_mute) {
	if (muted == p_mute) {
		return;
	}
	muted = p_mute;
	print_line("ControlServer::mute() - Muted: ", muted);
}

template <typename... Args>
void ControlServer::print_line_debug(Args... args) const {
	if (muted) {
		return;
	}
	print_line(args...);
}
