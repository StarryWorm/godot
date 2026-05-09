/**************************************************************************/
/*  gdtype.cpp                                                            */
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

#include "gdtype.h"

#include "core/os/memory.h"
#include "core/os/thread.h"
#include "core/variant/variant_internal.h"

GDType::GDType(const GDType *p_super_type, StringName p_name) :
		super_type(p_super_type), name(std::move(p_name)) {
	name_hierarchy.push_back(name);

	if (super_type) {
		for (const StringName &ancestor_name : super_type->name_hierarchy) {
			name_hierarchy.push_back(ancestor_name);
		}
	}
}

GDType::~GDType() {
	for (const KeyValue<StringName, const EnumInfo *> &kv : self_enum_map) {
		memdelete(const_cast<EnumInfo *>(kv.value));
	}
	for (const KeyValue<StringName, const MethodInfo *> &kv : self_signal_map) {
		memdelete(const_cast<MethodInfo *>(kv.value));
	}
}

void GDType::initialize() {
	ERR_FAIL_COND(init_state != InitState::UNINITIALIZED);

	if (super_type) {
		// Now that a subtype is registered, the supertype cannot change anymore.
		// Otherwise, our caches would become invalid.
		// This shouldn't be a problem, since classes should register all their
		// parts in _bind_methods, which is called on registration.
		super_type->init_state = InitState::FINALIZED;

		int_constant_map = super_type->int_constant_map;
		constant_map = super_type->constant_map;
		enum_map = super_type->enum_map;
		enum_cases_map = super_type->enum_cases_map;
		signal_map = super_type->signal_map;
	}

	init_state = InitState::MUTABLE;
}

void GDType::bind_enum(const StringName &p_enum, bool p_is_bitfield) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);
	ERR_FAIL_COND_MSG(p_enum.is_empty(), vformat("Enum name cannot be empty."));

	String enum_name = p_enum;
	if (enum_name.contains_char('.')) {
		enum_name = enum_name.get_slicec('.', 1);
	}

	// FIXME: Temporary solution as we currently do not bind enums directly, only their cases.
	// In the future this should throw an error instead of silently failing.
	// Requires compat-break for GDExtension.
	if (enum_map.has(enum_name)) {
		return;
	}

	EnumInfo *enum_info = memnew(EnumInfo);
	enum_info->name = enum_name;
	enum_info->is_bitfield = p_is_bitfield;
	enum_map[enum_name] = enum_info;
	self_enum_map[enum_name] = enum_info;
}

void GDType::bind_enum_case(const StringName &p_enum, const StringName &p_case, int64_t p_constant) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);
	ERR_FAIL_COND_MSG(p_enum.is_empty(), vformat("Enum name cannot be empty."));
	ERR_FAIL_COND_MSG(p_case.is_empty(), vformat("Enum case name cannot be empty."));
	ERR_FAIL_COND_MSG(enum_cases_map.has(p_case), vformat("Enum case '%s' already exists in class '%s'.", String(p_case), String(name)));
	ERR_FAIL_COND_MSG(constant_map.has(p_case), vformat("Class '%s' already has a constant named '%s'.", String(name), String(p_case)));

	String enum_name = p_enum;
	if (enum_name.contains_char('.')) {
		enum_name = enum_name.get_slicec('.', 1);
	}

	const EnumInfo **_enum_info = self_enum_map.getptr(enum_name);
	ERR_FAIL_COND_MSG(!_enum_info, vformat("Class '%s' does not have enum '%s'.", String(name), String(enum_name)));

	EnumInfo *enum_info = const_cast<EnumInfo *>(*_enum_info);
	enum_info->values.insert(p_case, p_constant);

	enum_cases_map.insert(p_case, p_constant);
	self_enum_cases_map.insert(p_case, p_constant);

	// FIXME: Temporary solution until all internal APIs fetch enums properly instead of relying on the int constant map
	int_constant_map[p_case] = p_constant;
	self_int_constant_map[p_case] = p_constant;
}

void GDType::bind_constant(const StringName &p_name, const Variant &p_constant) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);
	ERR_FAIL_COND_MSG(constant_map.has(p_name), vformat("Class '%s' already has constant '%s'.", String(name), String(p_name)));
	ERR_FAIL_COND_MSG(enum_cases_map.has(p_name), vformat("Class '%s' already has an enum case named '%s'.", String(name), String(p_name)));

	Variant::Type type = p_constant.get_type();
	String type_name = Variant::get_type_name(type);
	ERR_FAIL_COND_MSG(type == Variant::OBJECT || type == Variant::DICTIONARY || type_name.contains("ARRAY"),
			vformat("Class '%s': constant '%s' has unsupported type '%s' (Object, Dictionary, and array types are not allowed).", String(name), String(p_name), type_name));

	constant_map[p_name] = p_constant;
	self_constant_map[p_name] = p_constant;

	// FIXME: Temporary solution until we deprecate integer-specific constants.
	// Requires compat-break for GDExtension and ClassDB.
	// Remove `variant_internal.h` include when this is removed.
	if (type == Variant::INT) {
		int64_t value = *VariantInternal::get_int(&p_constant);
		int_constant_map[p_name] = value;
		self_int_constant_map[p_name] = value;
	}
}

const GDType::EnumInfo *GDType::get_integer_constant_enum(const StringName &p_name, bool p_no_inheritance) const {
	for (const KeyValue<StringName, const EnumInfo *> &kv : get_enum_map(p_no_inheritance)) {
		if (kv.value->values.has(p_name)) {
			return kv.value;
		}
	}

	return nullptr;
}

void GDType::add_signal(MethodInfo p_signal) {
	ERR_FAIL_COND(!Thread::is_main_thread());
	ERR_FAIL_COND(init_state != InitState::MUTABLE);

	const StringName signal_name(p_signal.name);
	ERR_FAIL_COND_MSG(signal_map.has(signal_name), vformat("Class '%s' already has signal '%s'.", String(name), String(signal_name)));

	const MethodInfo *ptr = memnew(MethodInfo(std::move(p_signal)));

	signal_map[signal_name] = ptr;
	self_signal_map[signal_name] = ptr;
}
