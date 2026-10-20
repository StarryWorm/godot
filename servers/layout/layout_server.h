/**************************************************************************/
/*  layout_server.h                                                       */
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

class LayoutServer : public Object {
	GDCLASS(LayoutServer, Object);

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

	using Axis = Vector2::Axis;

	enum SizeRequestMode {
		SIZE_REQUEST_CONSTANT, // Width and height are independent
		SIZE_REQUEST_HEIGHT_FOR_WIDTH, // Desired height depends on available width
		SIZE_REQUEST_WIDTH_FOR_HEIGHT // Desired width depends on available height
	};

	static Side begin_side(Axis p_axis) {
		return (p_axis == Axis::AXIS_X) ? SIDE_LEFT : SIDE_TOP;
	}
	static Side end_side(Axis p_axis) {
		return (p_axis == Axis::AXIS_X) ? SIDE_RIGHT : SIDE_BOTTOM;
	}

	static Axis primary_axis(SizeRequestMode p_mode) {
		return (p_mode == SIZE_REQUEST_WIDTH_FOR_HEIGHT) ? Axis::AXIS_Y : Axis::AXIS_X;
	}

private:
	static LayoutServer *singleton;

	enum DirtyFlag {
		DIRTY_NONE = 0,
		DIRTY_MAX_SIZE = 1 << 0,
		DIRTY_MIN_SIZE = 1 << 1,
		DIRTY_LAYOUT = 1 << 2,
		DIRTY_DRAW = 1 << 3,
		DIRTY_MAX_SIZE_PROPAGATION = 1 << 4,
	};

	/// Data management

	struct ControlLayoutCache {
		RID self;
		Control *control = nullptr;

		/// Layout tree

		RID parent;
		LocalVector<RID> children;
		int64_t depth = 0;

		bool is_internal_child = false;

		/// Children management

		LayoutMode default_children_layout_mode = LAYOUT_MODE_POSITION;

		/// Positioning and Sizing

		SizeRequestMode size_request_mode = SIZE_REQUEST_CONSTANT;
		LayoutMode layout_mode = LAYOUT_MODE_POSITION;

		GrowDirection h_grow = GROW_DIRECTION_END;
		GrowDirection v_grow = GROW_DIRECTION_END;

		Vector4 offsets;
		Vector4 anchors;

		Vector4 proposed_offsets;
		Vector4 proposed_anchors;

		Size2 custom_min_size;
		Size2 combined_min_size;

		Size2 custom_max_size = INVALID_SIZE;
		Size2 parent_maximum_size_cache = INVALID_SIZE;
		Size2 combined_max_size = INVALID_SIZE;

		Size2 inner_combined_max_size = INVALID_SIZE;

		bool propagate_maximum_size = false;

		AHashMap<real_t, real_t> desired_size_cache[2];

		/// State management

		BitField<DirtyFlag> dirty_flags = DIRTY_NONE;
		bool size_changed = false;
		bool position_changed = false;
	};
	mutable RID_Owner<ControlLayoutCache, true> cache_owner;

	/// Update management

	bool is_doing_layout = false;
	RID proposing_rid = RID();

	struct LayoutBucket {
		LocalVector<RID> dirty_min_sizes;
		LocalVector<RID> dirty_max_sizes;
		LocalVector<RID> dirty_layouts;

		void erase(RID p_rid) {
			dirty_min_sizes.erase(p_rid);
			dirty_max_sizes.erase(p_rid);
			dirty_layouts.erase(p_rid);
		}

		~LayoutBucket() {
			dirty_min_sizes.clear();
			dirty_max_sizes.clear();
			dirty_layouts.clear();
		}
	};
	AHashMap<uint64_t, LayoutBucket *> dirty_buckets;
	int64_t max_dirty_depth = 0;

	LocalVector<RID> notification_queue;
	LocalVector<RID> redraw_queue;
	LocalVector<Callable> callback_queue;

	void _clear_layout_buckets();

	// These methods return true if the value was updated.
	bool _update_min_size_cache(RID p_rid);
	bool _update_max_size_cache(RID p_rid);
	bool _update_desired_size_for_axis(RID p_rid, Axis p_axis, real_t p_for_size);

	real_t _get_desired_size_for_axis(RID p_rid, Axis p_axis, real_t p_for_size) const;

	Rect2 _get_parent_anchorable_rect(RID p_rid) const;

	void _compute_layout_rect(RID p_rid, Rect2 p_rect, bool p_keep_offsets);

	void _update_layout(RID p_rid);
	void _update_minimum_size(RID p_rid);
	void _update_maximum_size(RID p_rid);
	void _update_desired_size(RID p_rid);

	void _desired_sizes_pass(Axis p_axis, bool p_monotonic_clamping = false);
	void _layout_pass(Axis p_axis, bool p_monotonic_clamping = false);

	bool muted = true;

public:
	static LayoutServer *get_singleton() {
		ERR_FAIL_NULL_V_MSG(singleton, nullptr, "LayoutServer singleton is not initialized.");
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

	void set_size_request_mode(RID p_rid, SizeRequestMode p_mode);
	SizeRequestMode get_size_request_mode(RID p_rid) const;

	// Vector2 argument contains position along the axis first, then size along the axis.
	void set_proposed_layout(RID p_parent_rid, RID p_child_rid, Axis p_axis, Vector2 p_proposed_layout);

	Vector4 get_anchors(RID p_rid) const;
	void set_anchor(RID p_rid, Side p_side, real_t p_anchor, bool p_keep_offset, bool p_push_opposite_anchor);
	Vector4 get_offsets(RID p_rid) const;
	void set_offset(RID p_rid, Side p_side, real_t p_value);

	Size2 get_size(RID p_rid) const;
	void set_size(RID p_rid, Size2 p_size, bool p_keep_offsets);

	Size2 get_proposed_size(RID p_rid) const;

	Point2 get_position(RID p_rid) const;
	void set_position(RID p_rid, Point2 p_position, bool p_keep_offsets);

	Point2 get_proposed_position(RID p_rid) const;

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
	LayoutServer();
	~LayoutServer();
};

// Alias to make it easier to use
#define LS LayoutServer

// Notes:
// From now on, Controls cannot update both anchors/offsets and size/position simultaneously.
// This is unresolvable cleanly, and is not expected to be a compatibility issue in practice.
//
// TODO: Check if I reintroduced #24859
