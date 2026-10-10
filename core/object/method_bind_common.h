/**************************************************************************/
/*  method_bind_common.h                                                  */
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

#include "core/object/method_bind.h"
#include "core/variant/binder_common.h"

VARIANT_BITFIELD_CAST(MethodFlags)

/**** VARIADIC TEMPLATES ****/

#ifdef TYPED_METHOD_BIND
#define MB_T T
#else
class __UnexistingClass;
#define MB_T __UnexistingClass
#endif

/* INSTANCE BINDS */

template <typename T, bool IsConst, typename R, typename... P>
using MethodBindMethodPtr = std::conditional_t<IsConst, R (T::*)(P...) const, R (T::*)(P...)>;

template <typename T, bool IsConst, typename R, typename... P>
class MethodBindT : public MethodBind {
	using Method = MethodBindMethodPtr<T, IsConst, R, P...>;

private:
	static constexpr bool has_return = !std::is_void_v<R>;

	Method method;

	static MB_T *_cast(Object *p_object) {
#ifdef TYPED_METHOD_BIND
		return static_cast<MB_T *>(p_object);
#else
		return reinterpret_cast<MB_T *>(p_object);
#endif
	}

protected:
	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		}
		if constexpr (has_return) {
			return GetTypeInfo<R>::VARIANT_TYPE;
		} else {
			return Variant::NIL;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		if constexpr (has_return) {
			if (p_arg >= 0 && p_arg >= (int)sizeof...(P)) {
				return GetTypeInfo<R>::get_class_info();
			}
		}
		PropertyInfo pi;
		call_get_argument_type_info<P...>(p_arg, pi);
		return pi;
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		if constexpr (has_return) {
			if (p_arg < 0) {
				return GetTypeInfo<R>::METADATA;
			}
		}
		return call_get_argument_metadata<P...>(p_arg);
	}
#endif // DEBUG_ENABLED

	virtual Variant call(Object *p_object, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_V_MSG(p_object && p_object->is_extension_placeholder() && p_object->get_class_name() == get_instance_class(), Variant(), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		Variant ret;
		call_with_variant_args_dv(_cast(p_object), method, p_args, p_arg_count, ret, r_error, get_default_arguments());
		return ret;
	}

	virtual void validated_call(Object *p_object, const Variant **p_args, Variant *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(p_object && p_object->is_extension_placeholder() && p_object->get_class_name() == get_instance_class(), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_validated_args(_cast(p_object), method, p_args, r_ret);
	}

	virtual void ptrcall(Object *p_object, const void **p_args, void *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(p_object && p_object->is_extension_placeholder() && p_object->get_class_name() == get_instance_class(), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_ptr_args(_cast(p_object), method, p_args, r_ret);
	}

	MethodBindT(Method p_method) {
		method = p_method;
		_set_const(IsConst);
		_set_returns(has_return);
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
	}
};

template <typename T, bool IsConst, typename R, typename... P>
MethodBind *create_method_bind_internal(MethodBindMethodPtr<T, IsConst, R, P...> p_method) {
	using Bind = MethodBindT<MB_T, IsConst, R, P...>;
#ifdef TYPED_METHOD_BIND
	MethodBind *a = memnew(Bind(p_method));
#else
	MethodBind *a = memnew(Bind(reinterpret_cast<typename Bind::Method>(p_method)));
#endif
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename R, typename... P>
MethodBind *create_method_bind(R (T::*p_method)(P...)) {
	return create_method_bind_internal<T, false, R, P...>(p_method);
}

template <typename T, typename R, typename... P>
MethodBind *create_method_bind(R (T::*p_method)(P...) const) {
	return create_method_bind_internal<T, true, R, P...>(p_method);
}

/* STATIC BINDS */

template <typename R, typename... P>
class MethodBindTS : public MethodBind {
	static constexpr bool has_return = !std::is_void_v<R>;

	R (*function)(P...);

protected:
	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		}
		if constexpr (has_return) {
			return GetTypeInfo<R>::VARIANT_TYPE;
		} else {
			return Variant::NIL;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		if constexpr (has_return) {
			if (p_arg < 0 || p_arg >= (int)sizeof...(P)) {
				return GetTypeInfo<R>::get_class_info();
			}
		}
		PropertyInfo pi;
		call_get_argument_type_info<P...>(p_arg, pi);
		return pi;
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		if constexpr (has_return) {
			if (p_arg < 0) {
				return GetTypeInfo<R>::METADATA;
			}
		}
		return call_get_argument_metadata<P...>(p_arg);
	}

#endif // DEBUG_ENABLED
	virtual Variant call(Object *p_object, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		(void)p_object; // unused
		Variant ret;
		call_with_variant_args_dv(BINDER_NO_INSTANCE, function, p_args, p_arg_count, ret, r_error, get_default_arguments());
		return ret;
	}

	virtual void validated_call(Object *p_object, const Variant **p_args, Variant *r_ret) const override {
		call_with_validated_args(BINDER_NO_INSTANCE, function, p_args, r_ret);
	}

	virtual void ptrcall(Object *p_object, const void **p_args, void *r_ret) const override {
		(void)p_object;
		call_with_ptr_args(BINDER_NO_INSTANCE, function, p_args, r_ret);
	}

	MethodBindTS(void (*p_function)(P...)) {
		function = p_function;
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
		_set_static(true);
		_set_returns(has_return);
	}
};

template <typename R, typename... P>
MethodBind *create_static_method_bind(void (*p_method)(P...)) {
	MethodBind *a = memnew((MethodBindTS<R, P...>)(p_method));
	return a;
}
