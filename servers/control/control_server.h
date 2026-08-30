/**************************************************************************/
/*  control_server.h                                                      */
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

#pragma once

#include "core/error/error_macros.h"
#include "core/math/vector2.h"
#include "core/object/object.h"
#include "core/templates/a_hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/rid_owner.h"

class Control;
class Main;

class ControlServer : public Object {
	GDCLASS(ControlServer, Object);

public:
	static constexpr Size2 INVALID_SIZE = Size2(-1, -1);

	enum LayoutMode {
		LAYOUT_MODE_POSITION,
		LAYOUT_MODE_ANCHORS,
		LAYOUT_MODE_CONTAINER,
	};

	enum GrowDirection {
		GROW_DIRECTION_BEGIN,
		GROW_DIRECTION_END,
		GROW_DIRECTION_BOTH
	};

	enum Axis {
		AXIS_HORIZONTAL = 0,
		AXIS_VERTICAL = 1,
	};

private:
	static ControlServer *singleton;

	/// Data management

	struct ControlLayoutCache {
		/// Layout tree

		RID parent;
		LocalVector<RID> children;
		int64_t depth = 0;

		bool is_internal_child = false;

		/// Children management

		LayoutMode default_children_layout_mode = LAYOUT_MODE_POSITION;

		/// Positioning and Sizing

		LayoutMode layout_mode = LAYOUT_MODE_POSITION;

		GrowDirection h_grow = GROW_DIRECTION_END;
		GrowDirection v_grow = GROW_DIRECTION_END;

		// True representation of a Control's position and size in the layout.
		Vector4 offsets;
		Vector4 anchors;

		Vector4 new_offsets;
		Vector4 new_anchors;
		bool push_opposite_anchor[4] = { false, false, false, false };
		bool anchor_keep_offsets[4] = { false, false, false, false };

		Point2 new_position;
		Size2 new_size;
		bool new_rect_keep_offsets = false;
		bool explicit_position = false;
		bool explicit_size = false;
		Size2 last_user_defined_size;

		Size2 custom_min_size;
		Size2 combined_min_size;

		Size2 custom_max_size = INVALID_SIZE;
		Size2 parent_maximum_size_cache = INVALID_SIZE;
		Size2 combined_max_size = INVALID_SIZE;

		Size2 inner_combined_max_size = INVALID_SIZE;

		Size2 desired_size;

		bool propagate_maximum_size = false;

		/// State management

		bool min_size_dirty = false;
		bool max_size_dirty = false;
		bool desired_size_dirty = false;

		bool propagate_maximum_size_dirty = false;

		bool layout_dirty = false;
		bool state_changed = false;
		bool draw_dirty = false;

		bool size_changed = false;
		bool position_changed = false;
	};
	mutable RID_Owner<ControlLayoutCache, true> cache_owner;
	AHashMap<RID, Control *> managed_controls;

	/// Update management

	bool is_doing_layout = false;

	struct LayoutBucket {
		LocalVector<RID> dirty_min_sizes;
		LocalVector<RID> dirty_max_sizes;
		LocalVector<RID> dirty_desired_sizes;
		LocalVector<RID> dirty_layouts;
		LocalVector<RID> changed_states;

		void erase(RID p_rid) {
			dirty_min_sizes.erase(p_rid);
			dirty_max_sizes.erase(p_rid);
			dirty_desired_sizes.erase(p_rid);
			dirty_layouts.erase(p_rid);
			changed_states.erase(p_rid);
		}

		~LayoutBucket() {
			dirty_min_sizes.clear();
			dirty_max_sizes.clear();
			dirty_desired_sizes.clear();
			dirty_layouts.clear();
			changed_states.clear();
		}
	};
	AHashMap<uint64_t, LayoutBucket *> dirty_buckets;
	int64_t max_dirty_depth = 0;

	LocalVector<RID> queued_for_redraw;

	LocalVector<Callable> post_layout_callbacks;

	void _clear_layout_buckets();

	// These methods return true if the value was updated.
	bool _update_min_size_cache(RID p_rid);
	bool _update_max_size_cache(RID p_rid);
	bool _update_preferred_width_cache(RID p_rid);
	bool _update_desired_height_cache(RID p_rid);

	Rect2 _get_parent_anchorable_rect(RID p_rid) const;

	void _fit_uncontained_control(RID p_rid, Axis p_axis);
	void _fit_anchors(RID p_rid, Axis p_axis);
	void _set_anchor(RID p_rid, Side p_side);
	void _fit_control_rect(RID p_rid, Axis p_axis);
	void _fit_contained_control(RID p_rid, Axis p_axis);
	void _sync_anchor(RID p_rid, Side p_side);
	void _layout_contained_control(RID p_rid, Axis p_axis, real_t p_pos, real_t p_size);

	void _sync_new_anchors_and_offsets(RID p_rid, Axis p_axis);

	void _update_layout(RID p_rid);
	void _update_minimum_size(RID p_rid);
	void _update_maximum_size(RID p_rid);
	void _update_desired_size(RID p_rid);
	void _update_state(RID p_rid);

	bool muted = true;

public:
	static ControlServer *get_singleton() {
		ERR_FAIL_NULL_V_MSG(singleton, nullptr, "ControlServer singleton is not initialized.");
		return singleton;
	}

	/// Control tree management

	RID add_control(Control *p_control);
	void free_control(RID p_rid);

	void set_control_parent(RID p_rid, RID p_parent);
	void set_control_depth(RID p_rid, int p_depth);
	void set_control_as_internal_child(RID p_rid, bool p_internal);

	/// State management

	void update_visibility(RID p_rid);
	void update_layout(RID p_rid);
	void update_parent_layout(RID p_rid);
	void update_minimum_size(RID p_rid);
	void update_maximum_size(RID p_rid);
	void update_desired_size(RID p_rid);
	void queue_redraw(RID p_rid);

	/// Positioning and Sizing API

	void set_default_children_layout_mode(RID p_rid, LayoutMode p_mode);
	LayoutMode get_default_children_layout_mode(RID p_rid) const;

	void set_layout_mode(RID p_rid, LayoutMode p_mode);
	LayoutMode get_layout_mode(RID p_rid) const;
	LayoutMode get_default_layout_mode(RID p_rid) const;

	void set_h_grow_direction(RID p_rid, GrowDirection p_direction);
	GrowDirection get_h_grow_direction(RID p_rid) const;
	void set_v_grow_direction(RID p_rid, GrowDirection p_direction);
	GrowDirection get_v_grow_direction(RID p_rid) const;

	Vector4 get_anchors(RID p_rid) const;
	void set_anchor(RID p_rid, Side p_side, real_t p_anchor, bool p_keep_offset, bool p_push_opposite_anchor);
	Vector4 get_offsets(RID p_rid) const;
	void set_offset(RID p_rid, Side p_side, real_t p_value);

	Size2 get_size(RID p_rid) const;
	void set_size(RID p_rid, Size2 p_size, bool p_keep_offsets);

	Point2 get_position(RID p_rid) const;
	void set_position(RID p_rid, Point2 p_position, bool p_keep_offsets);

	void set_horizontal_layout(RID p_rid, real_t p_pos, real_t p_width);
	void set_vertical_layout(RID p_rid, real_t p_pos, real_t p_height);

	Size2 get_custom_minimum_size(RID p_rid) const;
	void set_custom_minimum_size(RID p_rid, Size2 p_size);
	Size2 get_custom_maximum_size(RID p_rid) const;
	void set_custom_maximum_size(RID p_rid, Size2 p_size);

	Size2 get_combined_minimum_size(RID p_rid) const;
	Size2 get_bound_minimum_size(RID p_rid) const;
	Size2 get_combined_maximum_size(RID p_rid) const;
	Size2 get_desired_size(RID p_rid) const;

	bool is_propagating_maximum_size(RID p_rid) const;
	void set_propagate_maximum_size(RID p_rid, bool p_propagate);

	// FIXME: Find a better way to do this?
	void set_parent_maximum_size_cache(RID p_rid, Size2 p_size);

	bool is_layout_dirty(RID p_rid) const;
	bool is_layout_dirty_in_tree(RID p_rid) const;

	/// Layout callbacks

	void add_post_layout_callback(const Callable &p_callable);

	/// Main loop

	void layout();

	void unmute() { mute(false); }
	void mute(bool p_mute = true);
	template <typename... Args>
	void print_line_debug(Args... args) const;

protected:
	// FIXME: Implement these two
	void _notification(int p_notification) {}
	static void _bind_methods() {}

public:
	ControlServer();
	~ControlServer();
};

// Alias to make it easier to use
#define CS ControlServer

// Notes:
// From now on, Controls cannot update both anchors/offsets and size/position simultaneously.
// This is unresolvable cleanly, and is not expected to be a compatibility issue in practice.
//
// TODO: Check if I reintroduced #24859
