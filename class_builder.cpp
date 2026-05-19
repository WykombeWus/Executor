// =============================================================================
//  Fluent Class<T> binding DSL for Lua
// -----------------------------------------------------------------------------
//  Goal: bind a C++ class to Lua with a single expression like
//
//      Class<Vector3>(L, "Vector3")
//          .constructor<double, double, double>()
//          .property("x", &Vector3::x)
//          .property("y", &Vector3::y)
//          .property("z", &Vector3::z)
//          .method  ("Magnitude", &Vector3::magnitude)
//          .method  ("Dot",       &Vector3::dot)
//          .meta    ("__add",      &Vector3::add)
//          .meta    ("__tostring", &Vector3::toString)
//          .end();
//
//  No hand-written __index, no hand-written __newindex, no hand-written
//  trampolines. Adding a new method or property is a single line.
//
//  How it works (one-paragraph version):
//    Member function pointers and member data pointers carry, in their type,
//    the class they belong to and the signature of what they access. We use
//    template metaprogramming (member_fn_traits / member_data_traits) to
//    extract those pieces, then generate per-pointer trampoline functions
//    that pull arguments from the Lua stack with the right types, invoke
//    the C++ method, and push the result. The actual member-pointer value
//    is stored as a Lua upvalue so one trampoline can serve any pointer of
//    a given type.
//
//  Build:  g++ -std=c++17 -Wall -Wextra class_builder.cpp -o class_builder -llua -lm -ldl
// =============================================================================

#include <cmath>
#include <cstring>
#include <iostream>
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

// =============================================================================
//  1. Userdata<T> -- carried over from the previous example, slightly trimmed.
// =============================================================================
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

// =============================================================================
//  2. Member-pointer traits
//
//  For a C++ member function pointer like `double (Vector3::*)() const`, we
//  want to extract:
//    - the class    (Vector3)
//    - the return   (double)
//    - the args     (std::tuple<>)
//
//  The standard pattern: declare a primary template, then specialize for
//  each cv-qualifier combination of the function part. We cover the four
//  common cases. Ref-qualified (& / &&) variants would be additional
//  specializations following the same pattern.
// =============================================================================
template <typename F> struct member_fn_traits;

template <typename C, typename R, typename... A>
struct member_fn_traits<R (C::*)(A...)> {
    using class_type  = C;
    using return_type = R;
    using args_tuple  = std::tuple<A...>;
};
template <typename C, typename R, typename... A>
struct member_fn_traits<R (C::*)(A...) const> {
    using class_type  = C;
    using return_type = R;
    using args_tuple  = std::tuple<A...>;
};
template <typename C, typename R, typename... A>
struct member_fn_traits<R (C::*)(A...) noexcept> {
    using class_type  = C;
    using return_type = R;
    using args_tuple  = std::tuple<A...>;
};
template <typename C, typename R, typename... A>
struct member_fn_traits<R (C::*)(A...) const noexcept> {
    using class_type  = C;
    using return_type = R;
    using args_tuple  = std::tuple<A...>;
};

// Member-data pointer traits: `double Vector3::*` -> { class=Vector3, field=double }.
template <typename F> struct member_data_traits;
template <typename C, typename V>
struct member_data_traits<V C::*> {
    using class_type = C;
    using field_type = V;
};

// =============================================================================
//  3. LuaConvert<T> -- the type-driven Lua <-> C++ marshaling table.
//
//  The default specialization assumes T is a userdata-bound class (it goes
//  through Userdata<T>). Specializations cover primitive Lua types so
//  numbers, bools, strings round-trip without writing any custom code.
//
//  A method `double Vector3::dot(const Vector3& o) const` will, after
//  std::decay_t, look up:
//    - LuaConvert<Vector3>     for the argument (-> Userdata<Vector3>::check)
//    - LuaConvert<double>      for the return    (-> lua_pushnumber)
// =============================================================================
template <typename T, typename = void>
struct LuaConvert {
    // Default: a userdata-bound class. Pull returns a reference into the
    // userdata so callers can pass it where T const& is expected without
    // an extra copy.
    static T& pull(lua_State* L, int idx) { return *Userdata<T>::check(L, idx); }
    static void push(lua_State* L, const T& v) { Userdata<T>::push(L, v); }
};

template <> struct LuaConvert<int> {
    static int  pull(lua_State* L, int i) { return static_cast<int>(luaL_checkinteger(L, i)); }
    static void push(lua_State* L, int  v) { lua_pushinteger(L, v); }
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

// Convenience: strip cv/ref before looking up the converter.
template <typename T>
using Converter = LuaConvert<std::decay_t<T>>;

// =============================================================================
//  4. Trampolines
//
//  Each one is a single C-style lua_CFunction. The actual member pointer is
//  carried per-binding as an upvalue, so the *function code* is generic and
//  the *function value* (a Lua closure) is specific to one binding.
// =============================================================================
namespace detail {

// Read a member pointer of type MP from upvalue 1 (full userdata).
template <typename MP>
static const MP& read_upvalue(lua_State* L) {
    return *static_cast<MP*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// Push `mp` as a full userdata so it can be the upvalue of a closure.
// Returns the pointer to the just-pushed copy (lifetime: as long as the
// closure that will capture it).
template <typename MP>
static void push_member_pointer(lua_State* L, MP mp) {
    void* mem = lua_newuserdata(L, sizeof(MP));
    new (mem) MP(mp);
    // No __gc needed: member pointers are trivially destructible.
}

// ----- Method trampoline ----------------------------------------------------
// Generated per member-function-pointer type. Pulls self, pulls args via
// LuaConvert, calls the method, pushes the result (or nothing for void).
template <typename MP, std::size_t... Is>
static int call_method(lua_State* L, std::index_sequence<Is...>) {
    using Tr   = member_fn_traits<MP>;
    using Cls  = typename Tr::class_type;
    using Ret  = typename Tr::return_type;
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
static int method_trampoline(lua_State* L) {
    constexpr std::size_t N = std::tuple_size_v<typename member_fn_traits<MP>::args_tuple>;
    return call_method<MP>(L, std::make_index_sequence<N>{});
}

// ----- Property getter / setter --------------------------------------------
template <typename MP>
static int property_getter(lua_State* L) {
    using Tr = member_data_traits<MP>;
    const MP& mp = read_upvalue<MP>(L);
    auto* self = Userdata<typename Tr::class_type>::check(L, 1);
    Converter<typename Tr::field_type>::push(L, self->*mp);
    return 1;
}

template <typename MP>
static int property_setter(lua_State* L) {
    using Tr = member_data_traits<MP>;
    const MP& mp = read_upvalue<MP>(L);
    auto* self = Userdata<typename Tr::class_type>::check(L, 1);
    self->*mp = Converter<typename Tr::field_type>::pull(L, 2);
    return 0;
}

// ----- Constructor trampoline ----------------------------------------------
// Invoked for `Vector3.new(3, 4, 12)`. Args... were specified by the user via
// .constructor<double, double, double>().
template <typename T, typename... Args, std::size_t... Is>
static int call_ctor(lua_State* L, std::index_sequence<Is...>) {
    Userdata<T>::push(L, Converter<Args>::pull(L, Is + 1)...);
    return 1;
}

template <typename T, typename... Args>
static int constructor_trampoline(lua_State* L) {
    return call_ctor<T, Args...>(L, std::index_sequence_for<Args...>{});
}

// ----- Generic __index ------------------------------------------------------
// Upvalues:
//   1: getters table  ({ "x" -> getter_closure, ... })
//   2: methods table  ({ "Magnitude" -> method_closure, ... })
//
// A read of `v.<key>` first checks getters; if found, calls it with `self`
// and returns the value. Otherwise falls back to methods[key], which is
// what enables `v:Magnitude()` (Lua's `:` desugars to two-step lookup).
static int generic_index(lua_State* L) {
    // Try getter first.
    lua_pushvalue(L, lua_upvalueindex(1));    // [..., getters]
    lua_pushvalue(L, 2);                       // [..., getters, key]
    lua_rawget(L, -2);                         // [..., getters, getters[key]]
    if (!lua_isnil(L, -1)) {
        // It's a closure. Call it as `getter(self)`.
        lua_pushvalue(L, 1);                   // [..., getters, getter, self]
        lua_call(L, 1, 1);                     // [..., getters, value]
        return 1;
    }
    lua_pop(L, 2);                             // discard nil + getters

    // Fall back to method table.
    lua_pushvalue(L, lua_upvalueindex(2));     // [..., methods]
    lua_pushvalue(L, 2);                       // [..., methods, key]
    lua_rawget(L, -2);                         // [..., methods, methods[key]]
    return 1;                                  // returns nil if no such name
}

// ----- Generic __newindex ---------------------------------------------------
// Upvalue 1: setters table. If no setter for `key`, raises an error so typos
// don't silently no-op.
static int generic_newindex(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));     // [..., setters]
    lua_pushvalue(L, 2);                       // [..., setters, key]
    lua_rawget(L, -2);                         // [..., setters, setter or nil]
    if (lua_isnil(L, -1)) {
        return luaL_error(L, "no settable field '%s'", luaL_checkstring(L, 2));
    }
    lua_pushvalue(L, 1);                       // self
    lua_pushvalue(L, 3);                       // value
    lua_call(L, 2, 0);
    return 0;
}

} // namespace detail

// =============================================================================
//  5. Class<T> -- the fluent builder.
//
//  All operations return *this so calls can be chained. The builder keeps
//  five tables alive on the stack until end() wires them together:
//
//      mt        - the metatable (registered in the Lua registry)
//      methods   - methods by name; consulted by __index when getter misses
//      getters   - property getters; consulted first by __index
//      setters   - property setters; consulted by __newindex
//      module    - the global table {new = ctor, ...} bound to T's name
//
//  The five tables are at fixed absolute stack indices for the builder's
//  lifetime, so we can lua_setfield into any of them without juggling.
// =============================================================================
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

    // -------- methods ------------------------------------------------------
    // `mp` is something like `&Vector3::magnitude`. We store its bytes as a
    // full-userdata upvalue and create a closure over the matching trampoline
    // template instantiation, then drop the closure into the methods table.
    template <typename MP>
    Class& method(const char* lua_name, MP mp) {
        detail::push_member_pointer<MP>(L_, mp);                  // [..., upv]
        lua_pushcclosure(L_, &detail::method_trampoline<MP>, 1);  // [..., closure]
        lua_setfield(L_, methods_, lua_name);
        return *this;
    }

    // -------- properties ---------------------------------------------------
    // Read/write property. Generates both a getter and a setter closure.
    template <typename MP>
    Class& property(const char* lua_name, MP mp) {
        // getter
        detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &detail::property_getter<MP>, 1);
        lua_setfield(L_, getters_, lua_name);
        // setter
        detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &detail::property_setter<MP>, 1);
        lua_setfield(L_, setters_, lua_name);
        return *this;
    }

    // Read-only property: only the getter is registered; writes raise via
    // generic_newindex's "no settable field" error.
    template <typename MP>
    Class& readonly(const char* lua_name, MP mp) {
        detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &detail::property_getter<MP>, 1);
        lua_setfield(L_, getters_, lua_name);
        return *this;
    }

    // -------- metamethods --------------------------------------------------
    // Bind a metamethod to a member function (e.g., __add -> &Vector3::add).
    template <typename MP>
    Class& meta(const char* meta_name, MP mp) {
        detail::push_member_pointer<MP>(L_, mp);
        lua_pushcclosure(L_, &detail::method_trampoline<MP>, 1);
        lua_setfield(L_, mt_, meta_name);
        return *this;
    }

    // Escape hatch: bind a metamethod to a raw lua_CFunction.
    Class& meta_raw(const char* meta_name, lua_CFunction f) {
        lua_pushcfunction(L_, f);
        lua_setfield(L_, mt_, meta_name);
        return *this;
    }

    // -------- constructor --------------------------------------------------
    // Generates Vector3.new(...) using std::index_sequence to unpack args.
    template <typename... Args>
    Class& constructor() {
        // Take the function pointer through a local because lua_pushcfunction
        // is a macro and would split the template argument list at the comma.
        lua_CFunction fn = &detail::constructor_trampoline<T, Args...>;
        lua_pushcfunction(L_, fn);
        lua_setfield(L_, module_, "new");
        return *this;
    }

    // -------- finish -------------------------------------------------------
    // Wire up __index, __newindex, __gc, bind module table to global, pop.
    void end() {
        // __index closure with upvalues (getters, methods)
        lua_pushvalue(L_, getters_);
        lua_pushvalue(L_, methods_);
        lua_pushcclosure(L_, &detail::generic_index, 2);
        lua_setfield(L_, mt_, "__index");

        // __newindex closure with upvalue (setters)
        lua_pushvalue(L_, setters_);
        lua_pushcclosure(L_, &detail::generic_newindex, 1);
        lua_setfield(L_, mt_, "__newindex");

        // __gc -- always provided by Userdata<T>
        lua_pushcfunction(L_, &Userdata<T>::gc);
        lua_setfield(L_, mt_, "__gc");

        // Bind module table as global _G[type_name]
        lua_pushvalue(L_, module_);
        lua_setglobal(L_, Userdata<T>::type_name);

        // Pop module, setters, getters, methods, mt -- 5 tables.
        lua_pop(L_, 5);
    }
};

// =============================================================================
//  6. Vector3 -- the C++ class to bind. Note: pure C++. No Lua glue here.
// =============================================================================
struct Vector3 {
    double x, y, z;

    Vector3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    double magnitude() const { return std::sqrt(x*x + y*y + z*z); }
    double dot(const Vector3& o) const { return x*o.x + y*o.y + z*o.z; }

    Vector3 add(const Vector3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector3 sub(const Vector3& o) const { return {x - o.x, y - o.y, z - o.z}; }

    std::string toString() const {
        return "Vector3(" + std::to_string(x) + ", "
                          + std::to_string(y) + ", "
                          + std::to_string(z) + ")";
    }
};

// =============================================================================
//  7. Driver -- the entire binding fits in ten lines.
// =============================================================================
int main() {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    Class<Vector3>(L, "Vector3")
        .constructor<double, double, double>()
        .property("x", &Vector3::x)
        .property("y", &Vector3::y)
        .property("z", &Vector3::z)
        .method  ("Magnitude",  &Vector3::magnitude)
        .method  ("Dot",        &Vector3::dot)
        .meta    ("__add",      &Vector3::add)
        .meta    ("__sub",      &Vector3::sub)
        .meta    ("__tostring", &Vector3::toString)
        .end();

    static const char* script = R"LUA(
        local v = Vector3.new(3, 4, 12)
        print(v.x, v.y, v.z)        -- 3   4   12
        print(v:Magnitude())        -- 13
        v.y = 50
        print(v.y)                  -- 50

        local w = Vector3.new(1, 2, 3)
        print(v:Dot(w))             -- 3 + 100 + 36 = 139
        print(v + w)                -- Vector3(4, 52, 15)
        print(v - w)
        print(tostring(v))

        -- Type safety: writes to undeclared fields fail loudly.
        local ok, err = pcall(function() v.bogus = 1 end)
        print("v.bogus = 1 ->", ok, err)
    )LUA";

    if (luaL_dostring(L, script) != LUA_OK) {
        std::cerr << "Lua error: " << lua_tostring(L, -1) << '\n';
        lua_pop(L, 1);
    }

    lua_close(L);
    return 0;
}
