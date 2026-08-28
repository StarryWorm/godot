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

#ifndef TYPED_METHOD_BIND
class __UnexistingClass;
#define MB_T __UnexistingClass
#else
#define MB_T T
#endif

template <typename T>
struct MethodBindObjectCaller {
	using InstanceType = T;

	static T *get(void *p_caller) {
		return static_cast<T *>(p_caller);
	}

	static T *get_ptrcall(void *p_caller) {
		return static_cast<T *>(p_caller);
	}

#ifdef TOOLS_ENABLED
	static bool is_extension_placeholder(void *p_caller, const StringName &p_instance_class) {
		Object *object = static_cast<Object *>(p_caller);
		return object && object->is_extension_placeholder() && object->get_class_name() == p_instance_class;
	}
#endif
};

template <typename T>
struct MethodBindVariantCaller {
	using InstanceType = T;

	static T *get(void *p_caller) {
		return &VariantInternalAccessor<T>::get(static_cast<Variant *>(p_caller));
	}

	static T *get_ptrcall(void *p_caller) {
		return static_cast<T *>(p_caller);
	}

#ifdef TOOLS_ENABLED
	static bool is_extension_placeholder(void *, const StringName &) {
		return false;
	}
#endif
};

// no return, not const
template <typename Caller, typename... P>
class MethodBindT : public MethodBind {
	using InstanceType = typename Caller::InstanceType;

protected:
	void (InstanceType::*method)(P...);

	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		} else {
			return Variant::NIL;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		PropertyInfo pi;
		call_get_argument_type_info<P...>(p_arg, pi);
		return pi;
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		return call_get_argument_metadata<P...>(p_arg);
	}

#endif // DEBUG_ENABLED
	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_V_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), Variant(), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_variant_args_dv(Caller::get(p_caller), method, p_args, p_arg_count, r_error, get_default_arguments());
		return Variant();
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_validated_object_instance_args(Caller::get(p_caller), method, p_args);
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_ptr_args<InstanceType, P...>(Caller::get_ptrcall(p_caller), method, p_args);
	}

	MethodBindT(void (InstanceType::*p_method)(P...)) {
		method = p_method;
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
	}
};

template <typename T, typename... P>
MethodBind *create_method_bind(void (T::*p_method)(P...)) {
#ifdef TYPED_METHOD_BIND
	MethodBind *a = memnew((MethodBindT<MethodBindObjectCaller<T>, P...>)(p_method));
#else
	MethodBind *a = memnew((MethodBindT<MethodBindObjectCaller<MB_T>, P...>)(reinterpret_cast<void (MB_T::*)(P...)>(p_method)));
#endif
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename... P>
MethodBind *create_variant_method_bind(void (T::*p_method)(P...)) {
	MethodBind *a = memnew((MethodBindT<MethodBindVariantCaller<T>, P...>)(p_method));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<T>::VARIANT_TYPE)));
	return a;
}

// no return, const

template <typename Caller, typename... P>
class MethodBindTC : public MethodBind {
	using InstanceType = typename Caller::InstanceType;

protected:
	void (InstanceType::*method)(P...) const;

	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		} else {
			return Variant::NIL;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		PropertyInfo pi;
		call_get_argument_type_info<P...>(p_arg, pi);
		return pi;
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		return call_get_argument_metadata<P...>(p_arg);
	}

#endif // DEBUG_ENABLED
	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_V_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), Variant(), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_variant_argsc_dv(Caller::get(p_caller), method, p_args, p_arg_count, r_error, get_default_arguments());
		return Variant();
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_validated_object_instance_argsc(Caller::get(p_caller), method, p_args);
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_ptr_argsc<InstanceType, P...>(Caller::get_ptrcall(p_caller), method, p_args);
	}

	MethodBindTC(void (InstanceType::*p_method)(P...) const) {
		method = p_method;
		_set_const(true);
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
	}
};

template <typename T, typename... P>
MethodBind *create_method_bind(void (T::*p_method)(P...) const) {
#ifdef TYPED_METHOD_BIND
	MethodBind *a = memnew((MethodBindTC<MethodBindObjectCaller<T>, P...>)(p_method));
#else
	MethodBind *a = memnew((MethodBindTC<MethodBindObjectCaller<MB_T>, P...>)(reinterpret_cast<void (MB_T::*)(P...) const>(p_method)));
#endif
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename... P>
MethodBind *create_variant_method_bind(void (T::*p_method)(P...) const) {
	MethodBind *a = memnew((MethodBindTC<MethodBindVariantCaller<T>, P...>)(p_method));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<T>::VARIANT_TYPE)));
	return a;
}

// return, not const

template <typename Caller, typename R, typename... P>
class MethodBindTR : public MethodBind {
	using InstanceType = typename Caller::InstanceType;

protected:
	R (InstanceType::*method)(P...);

	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		} else {
			return GetTypeInfo<R>::VARIANT_TYPE;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			PropertyInfo pi;
			call_get_argument_type_info<P...>(p_arg, pi);
			return pi;
		} else {
			return GetTypeInfo<R>::get_class_info();
		}
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		if (p_arg >= 0) {
			return call_get_argument_metadata<P...>(p_arg);
		} else {
			return GetTypeInfo<R>::METADATA;
		}
	}
#endif // DEBUG_ENABLED

	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		Variant ret;
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_V_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), ret, vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_variant_args_ret_dv(Caller::get(p_caller), method, p_args, p_arg_count, ret, r_error, get_default_arguments());
		return ret;
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_validated_object_instance_args_ret(Caller::get(p_caller), method, p_args, r_ret);
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_ptr_args_ret<InstanceType, R, P...>(Caller::get_ptrcall(p_caller), method, p_args, r_ret);
	}

	MethodBindTR(R (InstanceType::*p_method)(P...)) {
		method = p_method;
		_set_returns(true);
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
	}
};

template <typename T, typename R, typename... P>
MethodBind *create_method_bind(R (T::*p_method)(P...)) {
#ifdef TYPED_METHOD_BIND
	MethodBind *a = memnew((MethodBindTR<MethodBindObjectCaller<T>, R, P...>)(p_method));
#else
	MethodBind *a = memnew((MethodBindTR<MethodBindObjectCaller<MB_T>, R, P...>)(reinterpret_cast<R (MB_T::*)(P...)>(p_method)));
#endif

	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename R, typename... P>
MethodBind *create_variant_method_bind(R (T::*p_method)(P...)) {
	MethodBind *a = memnew((MethodBindTR<MethodBindVariantCaller<T>, R, P...>)(p_method));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<T>::VARIANT_TYPE)));
	return a;
}

// return, const

template <typename Caller, typename R, typename... P>
class MethodBindTRC : public MethodBind {
	using InstanceType = typename Caller::InstanceType;

protected:
	R (InstanceType::*method)(P...) const;

	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		} else {
			return GetTypeInfo<R>::VARIANT_TYPE;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			PropertyInfo pi;
			call_get_argument_type_info<P...>(p_arg, pi);
			return pi;
		} else {
			return GetTypeInfo<R>::get_class_info();
		}
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		if (p_arg >= 0) {
			return call_get_argument_metadata<P...>(p_arg);
		} else {
			return GetTypeInfo<R>::METADATA;
		}
	}
#endif // DEBUG_ENABLED

	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		Variant ret;
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_V_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), ret, vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_variant_args_retc_dv(Caller::get(p_caller), method, p_args, p_arg_count, ret, r_error, get_default_arguments());
		return ret;
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_validated_object_instance_args_retc(Caller::get(p_caller), method, p_args, r_ret);
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
#ifdef TOOLS_ENABLED
		ERR_FAIL_COND_MSG(Caller::is_extension_placeholder(p_caller, get_instance_class()), vformat("Cannot call method bind '%s' on placeholder instance.", MethodBind::get_name()));
#endif
		call_with_ptr_args_retc<InstanceType, R, P...>(Caller::get_ptrcall(p_caller), method, p_args, r_ret);
	}

	MethodBindTRC(R (InstanceType::*p_method)(P...) const) {
		method = p_method;
		_set_returns(true);
		_set_const(true);
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
	}
};

template <typename T, typename R, typename... P>
MethodBind *create_method_bind(R (T::*p_method)(P...) const) {
#ifdef TYPED_METHOD_BIND
	MethodBind *a = memnew((MethodBindTRC<MethodBindObjectCaller<T>, R, P...>)(p_method));
#else
	MethodBind *a = memnew((MethodBindTRC<MethodBindObjectCaller<MB_T>, R, P...>)(reinterpret_cast<R (MB_T::*)(P...) const>(p_method)));
#endif
	a->set_instance_class(T::get_class_static());
	return a;
}

template <typename T, typename R, typename... P>
MethodBind *create_variant_method_bind(R (T::*p_method)(P...) const) {
	MethodBind *a = memnew((MethodBindTRC<MethodBindVariantCaller<T>, R, P...>)(p_method));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<T>::VARIANT_TYPE)));
	return a;
}

// no return, not const

template <typename From, typename T, typename... P>
class MethodBindVariantConvertT : public MethodBindT<MethodBindVariantCaller<T>, P...> {
	using Base = MethodBindT<MethodBindVariantCaller<T>, P...>;

public:
	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		T converted(static_cast<T>(VariantInternalAccessor<From>::get(static_cast<Variant *>(p_caller))));
		call_with_variant_args_dv(&converted, this->method, p_args, p_arg_count, r_error, this->get_default_arguments());
		return Variant();
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		T converted(static_cast<T>(VariantInternalAccessor<From>::get(static_cast<Variant *>(p_caller))));
		call_with_validated_variant_args_helper<T, P...>(&converted, this->method, p_args, BuildIndexSequence<sizeof...(P)>{});
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		T converted(static_cast<T>(*reinterpret_cast<const From *>(p_caller)));
		call_with_ptr_args<T, P...>(&converted, this->method, p_args);
	}

	MethodBindVariantConvertT(void (T::*p_method)(P...)) :
			Base(p_method) {}
};

template <typename From, typename T, typename... P>
MethodBind *create_variant_method_bind(void (T::*p_method)(P...)) {
	MethodBind *a = memnew((MethodBindVariantConvertT<From, T, P...>)(p_method));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<From>::VARIANT_TYPE)));
	return a;
}

// no return, const

template <typename From, typename T, typename... P>
class MethodBindVariantConvertTC : public MethodBindTC<MethodBindVariantCaller<T>, P...> {
	using Base = MethodBindTC<MethodBindVariantCaller<T>, P...>;

public:
	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		T converted(static_cast<T>(VariantInternalAccessor<From>::get(static_cast<Variant *>(p_caller))));
		call_with_variant_argsc_dv(&converted, this->method, p_args, p_arg_count, r_error, this->get_default_arguments());
		return Variant();
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		T converted(static_cast<T>(VariantInternalAccessor<From>::get(static_cast<Variant *>(p_caller))));
		call_with_validated_variant_argsc_helper<T, P...>(&converted, this->method, p_args, BuildIndexSequence<sizeof...(P)>{});
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		T converted(static_cast<T>(*reinterpret_cast<const From *>(p_caller)));
		call_with_ptr_argsc<T, P...>(&converted, this->method, p_args);
	}

	MethodBindVariantConvertTC(void (T::*p_method)(P...) const) :
			Base(p_method) {}
};

template <typename From, typename T, typename... P>
MethodBind *create_variant_method_bind(void (T::*p_method)(P...) const) {
	MethodBind *a = memnew((MethodBindVariantConvertTC<From, T, P...>)(p_method));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<From>::VARIANT_TYPE)));
	return a;
}

// return, not const

template <typename From, typename T, typename R, typename... P>
class MethodBindVariantConvertTR : public MethodBindTR<MethodBindVariantCaller<T>, R, P...> {
	using Base = MethodBindTR<MethodBindVariantCaller<T>, R, P...>;

public:
	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		T converted(static_cast<T>(VariantInternalAccessor<From>::get(static_cast<Variant *>(p_caller))));
		Variant ret;
		call_with_variant_args_ret_dv(&converted, this->method, p_args, p_arg_count, ret, r_error, this->get_default_arguments());
		return ret;
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		T converted(static_cast<T>(VariantInternalAccessor<From>::get(static_cast<Variant *>(p_caller))));
		call_with_validated_variant_args_ret_helper<T, R, P...>(&converted, this->method, p_args, r_ret, BuildIndexSequence<sizeof...(P)>{});
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		T converted(static_cast<T>(*reinterpret_cast<const From *>(p_caller)));
		call_with_ptr_args_ret<T, R, P...>(&converted, this->method, p_args, r_ret);
	}

	MethodBindVariantConvertTR(R (T::*p_method)(P...)) :
			Base(p_method) {}
};

template <typename From, typename T, typename R, typename... P>
MethodBind *create_variant_method_bind(R (T::*p_method)(P...)) {
	MethodBind *a = memnew((MethodBindVariantConvertTR<From, T, R, P...>)(p_method));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<From>::VARIANT_TYPE)));
	return a;
}

// return, const

template <typename From, typename T, typename R, typename... P>
class MethodBindVariantConvertTRC : public MethodBindTRC<MethodBindVariantCaller<T>, R, P...> {
	using Base = MethodBindTRC<MethodBindVariantCaller<T>, R, P...>;

public:
	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		T converted(static_cast<T>(VariantInternalAccessor<From>::get(static_cast<Variant *>(p_caller))));
		Variant ret;
		call_with_variant_args_retc_dv(&converted, this->method, p_args, p_arg_count, ret, r_error, this->get_default_arguments());
		return ret;
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		T converted(static_cast<T>(VariantInternalAccessor<From>::get(static_cast<Variant *>(p_caller))));
		call_with_validated_variant_args_retc_helper<T, R, P...>(&converted, this->method, p_args, r_ret, BuildIndexSequence<sizeof...(P)>{});
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		T converted(static_cast<T>(*reinterpret_cast<const From *>(p_caller)));
		call_with_ptr_args_retc<T, R, P...>(&converted, this->method, p_args, r_ret);
	}

	MethodBindVariantConvertTRC(R (T::*p_method)(P...) const) :
			Base(p_method) {}
};

template <typename From, typename T, typename R, typename... P>
MethodBind *create_variant_method_bind(R (T::*p_method)(P...) const) {
	MethodBind *a = memnew((MethodBindVariantConvertTRC<From, T, R, P...>)(p_method));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<From>::VARIANT_TYPE)));
	return a;
}

/* STATIC BINDS */

// no return

template <typename... P>
class MethodBindTS : public MethodBind {
	void (*function)(P...);

protected:
	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		} else {
			return Variant::NIL;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		PropertyInfo pi;
		call_get_argument_type_info<P...>(p_arg, pi);
		return pi;
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		return call_get_argument_metadata<P...>(p_arg);
	}

#endif // DEBUG_ENABLED
	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		(void)p_caller; // unused
		call_with_variant_args_static_dv(function, p_args, p_arg_count, r_error, get_default_arguments());
		return Variant();
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		call_with_validated_variant_args_static_method(function, p_args);
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		(void)p_caller;
		(void)r_ret;
		call_with_ptr_args_static_method(function, p_args);
	}

	MethodBindTS(void (*p_function)(P...)) {
		function = p_function;
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
		_set_static(true);
	}
};

template <typename... P>
MethodBind *create_static_method_bind(void (*p_method)(P...)) {
	MethodBind *a = memnew((MethodBindTS<P...>)(p_method));
	return a;
}

// return

template <typename R, typename... P>
class MethodBindTRS : public MethodBind {
	R (*function)(P...);

protected:
	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		} else {
			return GetTypeInfo<R>::VARIANT_TYPE;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			PropertyInfo pi;
			call_get_argument_type_info<P...>(p_arg, pi);
			return pi;
		} else {
			return GetTypeInfo<R>::get_class_info();
		}
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		if (p_arg >= 0) {
			return call_get_argument_metadata<P...>(p_arg);
		} else {
			return GetTypeInfo<R>::METADATA;
		}
	}

#endif // DEBUG_ENABLED
	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		Variant ret;
		call_with_variant_args_static_ret_dv(function, p_args, p_arg_count, ret, r_error, get_default_arguments());
		return ret;
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		call_with_validated_variant_args_static_method_ret(function, p_args, r_ret);
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		(void)p_caller;
		call_with_ptr_args_static_method_ret(function, p_args, r_ret);
	}

	MethodBindTRS(R (*p_function)(P...)) {
		function = p_function;
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
		_set_static(true);
		_set_returns(true);
	}
};

template <typename R, typename... P>
MethodBind *create_static_method_bind(R (*p_method)(P...)) {
	MethodBind *a = memnew((MethodBindTRS<R, P...>)(p_method));
	return a;
}

// no return

template <typename T, bool p_const, typename... P>
class MethodBindVariantFunctionT : public MethodBind {
	void (*function)(T *, P...);

protected:
	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		} else {
			return Variant::NIL;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		PropertyInfo pi;
		call_get_argument_type_info<P...>(p_arg, pi);
		return pi;
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		return call_get_argument_metadata<P...>(p_arg);
	}
#endif // DEBUG_ENABLED

	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		T *instance = &VariantInternalAccessor<T>::get(static_cast<Variant *>(p_caller));
		call_with_variant_args_static_helper_dv(instance, function, p_args, p_arg_count, get_default_arguments(), r_error);
		return Variant();
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		call_with_validated_variant_args_static(static_cast<Variant *>(p_caller), function, p_args);
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		T *instance = reinterpret_cast<T *>(p_caller);
		call_with_ptr_args_static(instance, function, p_args);
	}

	MethodBindVariantFunctionT(void (*p_function)(T *, P...)) :
			function(p_function) {
		_set_const(p_const);
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
	}
};

template <typename T, bool p_const, typename... P>
MethodBind *create_variant_function_bind(void (*p_function)(T *, P...)) {
	MethodBind *a = memnew((MethodBindVariantFunctionT<T, p_const, P...>)(p_function));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<T>::VARIANT_TYPE)));
	return a;
}

// return

template <typename T, typename R, bool p_const, typename... P>
class MethodBindVariantFunctionTR : public MethodBind {
	R (*function)(T *, P...);

protected:
	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			return call_get_argument_type<P...>(p_arg);
		} else {
			return GetTypeInfo<R>::VARIANT_TYPE;
		}
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		if (p_arg >= 0 && p_arg < (int)sizeof...(P)) {
			PropertyInfo pi;
			call_get_argument_type_info<P...>(p_arg, pi);
			return pi;
		} else {
			return GetTypeInfo<R>::get_class_info();
		}
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int p_arg) const override {
		if (p_arg >= 0) {
			return call_get_argument_metadata<P...>(p_arg);
		} else {
			return GetTypeInfo<R>::METADATA;
		}
	}
#endif // DEBUG_ENABLED

	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		T *instance = &VariantInternalAccessor<T>::get(static_cast<Variant *>(p_caller));
		Variant ret;
		call_with_variant_args_retc_static_helper_dv(instance, function, p_args, p_arg_count, ret, get_default_arguments(), r_error);
		return ret;
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		call_with_validated_variant_args_static_retc(static_cast<Variant *>(p_caller), function, p_args, r_ret);
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		T *instance = reinterpret_cast<T *>(p_caller);
		call_with_ptr_args_static_retc(instance, function, p_args, r_ret);
	}

	MethodBindVariantFunctionTR(R (*p_function)(T *, P...)) :
			function(p_function) {
		_set_const(p_const);
		_generate_argument_types(sizeof...(P));
		set_argument_count(sizeof...(P));
		_set_returns(true);
	}
};

template <typename T, bool p_const, typename R, typename... P>
MethodBind *create_variant_function_bind(R (*p_function)(T *, P...)) {
	MethodBind *a = memnew((MethodBindVariantFunctionTR<T, R, p_const, P...>)(p_function));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<T>::VARIANT_TYPE)));
	return a;
}

template <typename T, typename R, bool p_has_return>
class MethodBindVariantVarArg : public MethodBind {
	using Function = void (*)(Variant *, const Variant **, int, Variant &, Callable::CallError &);

	Function function;
	MethodInfo method_info;

protected:
	virtual Variant::Type _gen_argument_type(int p_arg) const override {
		if (p_arg < 0) {
			return p_has_return ? GetTypeInfo<R>::VARIANT_TYPE : Variant::NIL;
		}
		if (p_arg < method_info.arguments.size()) {
			return method_info.arguments[p_arg].type;
		}
		return Variant::NIL;
	}

	virtual PropertyInfo _gen_argument_type_info(int p_arg) const override {
		if (p_arg < 0) {
			return p_has_return ? GetTypeInfo<R>::get_class_info() : PropertyInfo();
		}
		if (p_arg < method_info.arguments.size()) {
			return method_info.arguments[p_arg];
		}
		return PropertyInfo(Variant::NIL, "arg_" + itos(p_arg), PROPERTY_HINT_NONE, String(), PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT);
	}

public:
#ifdef DEBUG_ENABLED
	virtual GodotTypeInfo::Metadata get_argument_meta(int) const override {
		return GodotTypeInfo::METADATA_NONE;
	}
#endif // DEBUG_ENABLED

	virtual Variant call(void *p_caller, const Variant **p_args, int p_arg_count, Callable::CallError &r_error) const override {
		Variant ret;
		function(static_cast<Variant *>(p_caller), p_args, p_arg_count, ret, r_error);
		return ret;
	}

	virtual void validated_call(void *p_caller, const Variant **p_args, Variant *r_ret) const override {
		ERR_FAIL_MSG("Validated call can't be used with vararg methods. This is a bug.");
	}

	virtual void ptrcall(void *p_caller, const void **p_args, void *r_ret) const override {
		ERR_FAIL_MSG("ptrcall can't be used with vararg methods. This is a bug.");
	}

	virtual bool is_vararg() const override {
		return true;
	}

	MethodBindVariantVarArg(Function p_function, const MethodInfo &p_method_info) :
			function(p_function), method_info(p_method_info) {
		_set_const(true);
		_set_returns(p_has_return);
		set_argument_count(method_info.arguments.size());
		_generate_argument_types(method_info.arguments.size());
	}
};

template <typename T, typename R, bool p_has_return>
MethodBind *create_variant_vararg_bind(void (*p_function)(Variant *, const Variant **, int, Variant &, Callable::CallError &), const MethodInfo &p_method_info) {
	MethodBind *a = memnew((MethodBindVariantVarArg<T, R, p_has_return>)(p_function, p_method_info));
	a->set_instance_class(StringName(Variant::get_type_name(GetTypeInfo<T>::VARIANT_TYPE)));
	return a;
}

template <typename T>
MethodBind *create_variant_vararg_bind(void (*p_function)(Variant *, const Variant **, int, Variant &, Callable::CallError &), Variant::Type p_arg_type, const String &p_arg_name) {
	MethodInfo method_info;
	method_info.arguments.push_back(PropertyInfo(p_arg_type, p_arg_name));
	return create_variant_vararg_bind<T, Variant, false>(p_function, method_info);
}
