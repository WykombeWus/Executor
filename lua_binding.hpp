// =============================================================================
//  lua_binding.hpp -- Userdata<T> + Class<T> fluent binding for Lua 5.4.
//
//  Extracted from class_builder.cpp so multiple translation units (including
//  the sandbox example) can share the same binding machinery.
//
//  The four pieces:
//    Userdata<T>            - typed userdata allocation, validation, GC.
//    member_*_traits        - extract signatures from member pointers.
//    LuaConvert<T>          - type-driven Lua <-> C++ marshaling.
//    Class<T>               - fluent registration DSL.
// =============================================================================
#pragma once

#include <cmath>
#include <new>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

extern "C" {
    #include <lua.h>
    #include <lualib.h>
    #include <lauxlib.h>
}

// -----------------------------------------------------------------------------
// Userdata<T>
// -----------------------------------------------------------------------------
template <typename T>
class Userdata {
public:
    inline static const char* type_name = nullptr;

    template <typename... Args>
    static T* push(lua_State* L, Args&&... args) {
        void* mem = lua_newuserdata(L, sizeof(T));
        T* obj = new (mem) T(std::forward<Args>(args)...);
        luaL_setmetatable(L, type_name);
        return obj;
    }

    static T* check(lua_State* L, int idx) {
        return static_cast<T*>(luaL_checkudata(L, idx, type_name));
    }

    static int gc(lua_State* L) {
        check(L, 1)->~T();
        return 0;
    }
};

// -----------------------------------------------------------------------------
// Member-pointer traits
// -----------------------------------------------------------------------------
template <typename F> struct member_fn_traits;

template <typename C, typename R, typename... A>
struct member_fn_traits<R (C::*)(A...)> {
    using class_type = C; using return_type = R; using args_tuple = std::tuple<A...>;
};
template <typename C, typename R, typename... A>
struct member_fn_traits<R (C::*)(A...) const> {
    using class_type = C; using return_type = R; using args_tuple = std::tuple<A...>;
};
template <typename C, typename R, typename... A>
struct member_fn_traits<R (C::*)(A...) noexcept> {
    using class_type = C; using return_type = R; using args_tuple = std::tuple<A...>;
};
template <typename C, typename R, typename... A>
struct member_fn_traits<R (C::*)(A...) const noexcept> {
    using class_type = C; using return_type = R; using args_tuple = std::tuple<A...>;
};

template <typename F> struct member_data_traits;
template <typename C, typename V>
struct member_data_traits<V C::*> { using class_type = C; using field_type = V; };

// -----------------------------------------------------------------------------
// LuaConvert<T>
// -----------------------------------------------------------------------------
template <typename T, typename = void>
struct LuaConvert {
    static T& pull(lua_State* L, int idx) { return *Userdata<T>::check(L, idx); }
    static void push(lua_State* L, const T& v) { Userdata<T>::push(L, v); }
};

template <> struct LuaConvert<int> {
    static int  pull(lua_State* L, int i) { return static_cast<int>(luaL_checkinteger(L, i)); }
    static void push(lua_State* L, int v) { lua_pushinteger(L, v); }
};
template <> struct LuaConvert<long> {
    static long pull(lua_State* L, int i) { return static_cast<long>(luaL_checkinteger(L, i)); }
    static void push(lua_State* L, long v) { lua_pushinteger(L, v); }
};
template <> struct LuaConvert<double> {
    static double pull(lua_State* L, int i) { return luaL_checknumber(L, i); }
    static void   push(lua_State* L, double v) { lua_pushnumber(L, v); }
};
template <> struct LuaConvert<float> {
    static float pull(lua_State* L, int i) { return static_cast<float>(luaL_checknumber(L, i)); }
    static void  push(lua_State* L, float v) { lua_pushnumber(L, v); }
};
template <> struct LuaConvert<bool> {
    static bool pull(lua_State* L, int i) { return lua_toboolean(L, i) != 0; }
    static void push(lua_State* L, bool v) { lua_pushboolean(L, v ? 1 : 0); }
};
template <> struct LuaConvert<const char*> {
    static const char* pull(lua_State* L, int i) { return luaL_checkstring(L, i); }
    static void        push(lua_State* L, const char* v) { lua_pushstring(L, v ? v : ""); }
};
template <> struct LuaConvert<std::string> {
    static std::string pull(lua_State* L, int i) {
        size_t n; const char* s = luaL_checklstring(L, i, &n);
        return std::string(s, n);
    }
    static void push(lua_State* L, const std::string& v) {
        lua_pushlstring(L, v.data(), v.size());
    }
};

template <typename T>
using Converter = LuaConvert<std::decay_t<T>>;

// -----------------------------------------------------------------------------
// Trampolines (in lua_detail namespace - implementation, not API)
// -----------------------------------------------------------------------------
namespace lua_detail {

template <typename MP>
inline const MP& read_upvalue(lua_State* L) {
    return *static_cast<MP*>(lua_touserdata(L, lua_upvalueindex(1)));
}

template <typename MP>
inline void push_member_pointer(lua_State* L, MP mp) {
    void* mem = lua_newuserdata(L, sizeof(MP));
    new (mem) MP(mp);
}

template <typename MP, std::size_t... Is>
inline int call_method(lua_State* L, std::index_sequence<Is...>) {
    using Tr = member_fn_traits<MP>;
    using Cls = typename Tr::class_type;
    using Ret = typename Tr::return_type;
    using Args = typename Tr::args_tuple;

    const MP& mp = read_upvalue<MP>(L);
    Cls* self = Userdata<Cls>::check(L, 1);

    if constexpr (std::is_void_v<Ret>) {
        (self->*mp)( Converter<std::tuple_element_t<Is, Args>>::pull(L, Is + 2)... );
        return 0;
    } else {
        decltype(auto) result =
            (self->*mp)( Converter<std::tuple_element_t<Is, Args>>::pull(L, Is + 2)... );
        Converter<Ret>::push(L, result);
        return 1;
    }
}

template <typename MP>
inline int method_trampoline(lua_State* L) {
    constexpr std::size_t N = std::tuple_size_v<typename member_fn_traits<MP>::args_tuple>;
    return call_method<MP>(L, std::make_index_sequence<N>{});
}

template <typename MP>
inline int property_getter(lua_State* L) {
    using Tr = member_data_traits<MP>;
    const MP& mp = read_upvalue<MP>(L);
    auto* self = Userdata<typename Tr::class_type>::check(L, 1);
    Converter<typename Tr::field_type>::push(L, self->*mp);
    return 1;
}

template <typename MP>
inline int property_setter(lua_State* L) {
    using Tr = member_data_traits<MP>;
    const MP& mp = read_upvalue<MP>(L);
    auto* self = Userdata<typename Tr::class_type>::check(L, 1);
    self->*mp = Converter<typename Tr::field_type>::pull(L, 2);
    return 0;
}

template <typename T, typename... Args, std::size_t... Is>
inline int call_ctor(lua_State* L, std::index_sequence<Is...>) {
    Userdata<T>::push(L, Converter<Args>::pull(L, Is + 1)...);
    return 1;
}

template <typename T, typename... Args>
inline int constructor_trampoline(lua_State* L) {
    return call_ctor<T, Args...>(L, std::index_sequence_for<Args...>{});
}

inline int generic_index(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) {
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        return 1;
    }
    lua_pop(L, 2);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

inline int generic_newindex(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "no settable field '%s'", luaL_checkstring(L, 2));
    }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 3);
    lua_call(L, 2, 0);
    return 0;
}

} // namespace lua_detail

// -----------------------------------------------------------------------------
// Class<T> -- fluent builder
// -----------------------------------------------------------------------------
template <typename T>
class Class {
    lua_State* L_;
    int mt_, methods_, getters_, setters_, module_;

public:
    Class(lua_State* L, const char* name) : L_(L) {
        Userdata<T>::type_name = name;
        luaL_newmetatable(L_, name);  mt_      = lua_gettop(L_);
        lua_newtable(L_);             methods_ = lua_gettop(L_);
        lua_newtable(L_);             getters_ = lua_gettop(L_);
        lua_newtable(L_);             setters_ = lua_gettop(L_);
        lua_newtable(L_);             module_  = lua_gettop(L_);
    }

    template <typename MP>
    Class& method(const char* lua_name, MP mp) {
        lua_detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &lua_detail::method_trampoline<MP>, 1);
        lua_setfield(L_, methods_, lua_name);
        return *this;
    }

    template <typename MP>
    Class& property(const char* lua_name, MP mp) {
        lua_detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &lua_detail::property_getter<MP>, 1);
        lua_setfield(L_, getters_, lua_name);
        lua_detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &lua_detail::property_setter<MP>, 1);
        lua_setfield(L_, setters_, lua_name);
        return *this;
    }

    template <typename MP>
    Class& readonly(const char* lua_name, MP mp) {
        lua_detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &lua_detail::property_getter<MP>, 1);
        lua_setfield(L_, getters_, lua_name);
        return *this;
    }

    template <typename MP>
    Class& meta(const char* meta_name, MP mp) {
        lua_detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &lua_detail::method_trampoline<MP>, 1);
        lua_setfield(L_, mt_, meta_name);
        return *this;
    }

    Class& meta_raw(const char* meta_name, lua_CFunction f) {
        lua_pushcfunction(L_, f);
        lua_setfield(L_, mt_, meta_name);
        return *this;
    }

    template <typename... Args>
    Class& constructor() {
        lua_CFunction fn = &lua_detail::constructor_trampoline<T, Args...>;
        lua_pushcfunction(L_, fn);
        lua_setfield(L_, module_, "new");
        return *this;
    }

    void end() {
        lua_pushvalue(L_, getters_);
        lua_pushvalue(L_, methods_);
        lua_pushcclosure(L_, &lua_detail::generic_index, 2);
        lua_setfield(L_, mt_, "__index");

        lua_pushvalue(L_, setters_);
        lua_pushcclosure(L_, &lua_detail::generic_newindex, 1);
        lua_setfield(L_, mt_, "__newindex");

        lua_pushcfunction(L_, &Userdata<T>::gc);
        lua_setfield(L_, mt_, "__gc");

        lua_pushvalue(L_, module_);
        lua_setglobal(L_, Userdata<T>::type_name);

        lua_pop(L_, 5);
    }
};
