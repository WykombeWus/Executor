// =============================================================================
//  Generic Userdata<T> binding helper for Lua
// -----------------------------------------------------------------------------
//  Builds on the previous luaL_Reg / luaL_setfuncs example by lifting the
//  per-class boilerplate (push, check, __gc, registration plumbing) into a
//  single class template. The result: binding a new C++ class becomes
//
//     1. write the C functions (use Userdata<T>::check / ::push for plumbing)
//     2. declare a luaL_Reg methods[] and a luaL_Reg metamethods[]
//     3. one call to Userdata<T>::register_class
//
//  This file binds *two* classes (Vector3 and Quaternion) so you can see how
//  little the second one costs once the template exists.
//
//  Build:  g++ -std=c++17 -Wall -Wextra userdata_template.cpp -o userdata_template -llua -lm -ldl
// =============================================================================

#include <cmath>
#include <cstring>
#include <iostream>
#include <new>
#include <utility>      // std::forward

extern "C" {
    #include <lua.h>
    #include <lualib.h>
    #include <lauxlib.h>
}

// =============================================================================
//  Userdata<T> -- generic binding helper.
//
//  Responsibilities:
//    * Allocate Lua-managed memory and placement-new a T into it (push).
//    * Type-check a stack slot and return T* (check).
//    * Provide a default __gc that runs ~T() exactly once.
//    * Provide register_class() that wires up:
//        - the metatable (registered under the user-provided type name)
//        - a methods table containing user methods
//        - user metamethods, with the methods table as their shared upvalue
//        - the auto-generated __gc
//        - a module table (constructor etc.) bound to a global Lua name
//
//  Type safety:
//    * Each instantiation Userdata<T> stores its own type name in an inline
//      static, set once during register_class. luaL_checkudata compares this
//      exact key, so passing a Quaternion to a Vector3 method raises a clean
//      Lua error - never a memory-unsafe pointer cast.
//    * push() variadically forwards arguments to T's constructor, so the
//      compiler refuses any push that wouldn't compile as `T(args...)`.
//    * check() returns T*, not void*, so every call site is statically typed.
// =============================================================================
template <typename T>
class Userdata {
public:
    // The Lua-visible type name. Doubles as the metatable key in the registry
    // and is what luaL_checkudata uses for "Vector3 expected, got Quaternion"
    // error messages. Set once by register_class; templates ensure each T has
    // its own independent storage.
    inline static const char* type_name = nullptr;

    // Allocate a new T inside Lua-managed userdata and push it on the stack.
    // Returns a typed pointer for any further C++-side initialization.
    template <typename... Args>
    static T* push(lua_State* L, Args&&... args)
    {
        void* mem = lua_newuserdata(L, sizeof(T));
        T* obj = new (mem) T(std::forward<Args>(args)...);
        // luaL_setmetatable (Lua 5.2+) is shorthand for getmetatable+setmetatable.
        luaL_setmetatable(L, type_name);
        return obj;
    }

    // Validate that the value at `idx` is a userdata with our metatable and
    // return a typed pointer. Raises a Lua error on mismatch.
    static T* check(lua_State* L, int idx)
    {
        return static_cast<T*>(luaL_checkudata(L, idx, type_name));
    }

    // Generic __gc: runs ~T() so RAII members (std::string, std::vector,
    // std::unique_ptr, file handles, ...) clean themselves up correctly.
    static int gc(lua_State* L)
    {
        check(L, 1)->~T();
        return 0;
    }

    // Register the class. Call once during host setup.
    //
    //   lua_name      - global name AND metatable key, e.g. "Vector3"
    //   methods       - luaL_Reg array; entries become v:Method() calls
    //   metamethods   - luaL_Reg array; __index, __newindex, __add, ...
    //                   The methods table is passed as their shared upvalue 1.
    //   module_funcs  - luaL_Reg array of module-level functions like `new`.
    //                   Bound as _G[lua_name].
    //
    // All array pointers may be nullptr to skip a section.
    static void register_class(lua_State* L,
                               const char* lua_name,
                               const luaL_Reg* methods,
                               const luaL_Reg* metamethods,
                               const luaL_Reg* module_funcs)
    {
        type_name = lua_name;

        // 1. Metatable, keyed by lua_name in the registry.
        luaL_newmetatable(L, type_name);                // [mt]

        // 2. Methods table.
        lua_newtable(L);                                // [mt, methods]
        if (methods) {
            luaL_setfuncs(L, methods, 0);               // [mt, methods]
        }

        // 3. Metamethods registered into mt, with methods as shared upvalue 1.
        //    luaL_setfuncs pops the upvalue when finished.
        if (metamethods) {
            luaL_setfuncs(L, metamethods, 1);           // [mt]
        } else {
            lua_pop(L, 1);                              // [mt]
        }

        // 4. Auto-provide __gc. We set this *after* user metamethods so it
        //    overrides any user attempt to define __gc - that's intentional:
        //    custom cleanup belongs in T's destructor, not in __gc.
        lua_pushcfunction(L, &Userdata<T>::gc);
        lua_setfield(L, -2, "__gc");

        lua_pop(L, 1);                                  // []

        // 5. Module table bound to the global lua_name (e.g. _G.Vector3).
        //    We can't use luaL_newlib here because it's a macro that takes
        //    sizeof of the literal array - useless on a pointer parameter.
        //    Count manually and call lua_createtable + luaL_setfuncs.
        if (module_funcs && lua_name) {
            int n = 0;
            while (module_funcs[n].name != nullptr) ++n;
            lua_createtable(L, 0, n);                   // [module]
            luaL_setfuncs(L, module_funcs, 0);          // [module]
            lua_setglobal(L, lua_name);                 // []
        }
    }
};

// =============================================================================
//  Class #1: Vector3
// =============================================================================
struct Vector3 {
    double x, y, z;
    Vector3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    double magnitude() const { return std::sqrt(x*x + y*y + z*z); }
    double dot(const Vector3& o) const { return x*o.x + y*o.y + z*o.z; }
};

// Methods - all use Userdata<Vector3>::check for type-safe extraction.
static int v3_magnitude(lua_State* L)
{
    auto* v = Userdata<Vector3>::check(L, 1);
    lua_pushnumber(L, v->magnitude());
    return 1;
}

static int v3_dot(lua_State* L)
{
    auto* a = Userdata<Vector3>::check(L, 1);
    auto* b = Userdata<Vector3>::check(L, 2);
    lua_pushnumber(L, a->dot(*b));
    return 1;
}

static int v3_normalize(lua_State* L)
{
    auto* v = Userdata<Vector3>::check(L, 1);
    double m = v->magnitude();
    if (m == 0.0) return luaL_error(L, "cannot normalize zero vector");
    Userdata<Vector3>::push(L, v->x/m, v->y/m, v->z/m);
    return 1;
}

// Metamethods
static int v3_index(lua_State* L)
{
    auto* v = Userdata<Vector3>::check(L, 1);
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "x") == 0) { lua_pushnumber(L, v->x); return 1; }
    if (std::strcmp(k, "y") == 0) { lua_pushnumber(L, v->y); return 1; }
    if (std::strcmp(k, "z") == 0) { lua_pushnumber(L, v->z); return 1; }
    // Fall back to methods (stored as shared upvalue 1 by register_class).
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

static int v3_newindex(lua_State* L)
{
    auto* v = Userdata<Vector3>::check(L, 1);
    const char* k = luaL_checkstring(L, 2);
    double val = luaL_checknumber(L, 3);
    if (std::strcmp(k, "x") == 0) { v->x = val; return 0; }
    if (std::strcmp(k, "y") == 0) { v->y = val; return 0; }
    if (std::strcmp(k, "z") == 0) { v->z = val; return 0; }
    return luaL_error(L, "Vector3 has no field '%s'", k);
}

static int v3_tostring(lua_State* L)
{
    auto* v = Userdata<Vector3>::check(L, 1);
    lua_pushfstring(L, "Vector3(%f, %f, %f)", v->x, v->y, v->z);
    return 1;
}

static int v3_add(lua_State* L)
{
    auto* a = Userdata<Vector3>::check(L, 1);
    auto* b = Userdata<Vector3>::check(L, 2);
    Userdata<Vector3>::push(L, a->x + b->x, a->y + b->y, a->z + b->z);
    return 1;
}

// Module-level constructor.
static int v3_new(lua_State* L)
{
    Userdata<Vector3>::push(L,
        luaL_checknumber(L, 1),
        luaL_checknumber(L, 2),
        luaL_checknumber(L, 3));
    return 1;
}

static const luaL_Reg Vector3_methods[] = {
    {"Magnitude", v3_magnitude},
    {"Dot",       v3_dot},
    {"Normalize", v3_normalize},
    {nullptr, nullptr}
};

static const luaL_Reg Vector3_metamethods[] = {
    {"__index",    v3_index},
    {"__newindex", v3_newindex},
    {"__tostring", v3_tostring},
    {"__add",      v3_add},
    // Note: no __gc - Userdata<Vector3>::register_class auto-provides it.
    {nullptr, nullptr}
};

static const luaL_Reg Vector3_module[] = {
    {"new", v3_new},
    {nullptr, nullptr}
};

// =============================================================================
//  Class #2: Quaternion -- to show how cheap the *second* class is.
//
//  All the boilerplate (push/check/__gc) comes for free from Userdata<T>.
//  Everything below is genuine class behavior, not binding plumbing.
// =============================================================================
struct Quaternion {
    double w, x, y, z;
    Quaternion(double w_, double x_, double y_, double z_)
        : w(w_), x(x_), y(y_), z(z_) {}

    double magnitude() const { return std::sqrt(w*w + x*x + y*y + z*z); }

    Quaternion conjugate() const { return {w, -x, -y, -z}; }

    Quaternion operator*(const Quaternion& o) const {
        return {
            w*o.w - x*o.x - y*o.y - z*o.z,
            w*o.x + x*o.w + y*o.z - z*o.y,
            w*o.y - x*o.z + y*o.w + z*o.x,
            w*o.z + x*o.y - y*o.x + z*o.w
        };
    }
};

static int q_magnitude(lua_State* L)
{
    lua_pushnumber(L, Userdata<Quaternion>::check(L, 1)->magnitude());
    return 1;
}

static int q_conjugate(lua_State* L)
{
    auto* q = Userdata<Quaternion>::check(L, 1);
    Quaternion c = q->conjugate();
    Userdata<Quaternion>::push(L, c.w, c.x, c.y, c.z);
    return 1;
}

static int q_index(lua_State* L)
{
    auto* q = Userdata<Quaternion>::check(L, 1);
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "w") == 0) { lua_pushnumber(L, q->w); return 1; }
    if (std::strcmp(k, "x") == 0) { lua_pushnumber(L, q->x); return 1; }
    if (std::strcmp(k, "y") == 0) { lua_pushnumber(L, q->y); return 1; }
    if (std::strcmp(k, "z") == 0) { lua_pushnumber(L, q->z); return 1; }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

static int q_tostring(lua_State* L)
{
    auto* q = Userdata<Quaternion>::check(L, 1);
    lua_pushfstring(L, "Quaternion(%f, %f, %f, %f)", q->w, q->x, q->y, q->z);
    return 1;
}

static int q_mul(lua_State* L)
{
    auto* a = Userdata<Quaternion>::check(L, 1);
    auto* b = Userdata<Quaternion>::check(L, 2);
    Quaternion r = (*a) * (*b);
    Userdata<Quaternion>::push(L, r.w, r.x, r.y, r.z);
    return 1;
}

static int q_new(lua_State* L)
{
    Userdata<Quaternion>::push(L,
        luaL_checknumber(L, 1),
        luaL_checknumber(L, 2),
        luaL_checknumber(L, 3),
        luaL_checknumber(L, 4));
    return 1;
}

static const luaL_Reg Quaternion_methods[] = {
    {"Magnitude", q_magnitude},
    {"Conjugate", q_conjugate},
    {nullptr, nullptr}
};

static const luaL_Reg Quaternion_metamethods[] = {
    {"__index",    q_index},
    {"__tostring", q_tostring},
    {"__mul",      q_mul},
    {nullptr, nullptr}
};

static const luaL_Reg Quaternion_module[] = {
    {"new", q_new},
    {nullptr, nullptr}
};

// =============================================================================
//  Driver
// =============================================================================
int main()
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    // Each class is now one line of registration.
    Userdata<Vector3>::register_class(L,
        "Vector3", Vector3_methods, Vector3_metamethods, Vector3_module);

    Userdata<Quaternion>::register_class(L,
        "Quaternion", Quaternion_methods, Quaternion_metamethods, Quaternion_module);

    static const char* script = R"LUA(
        -- Vector3 still works as before
        local v = Vector3.new(3, 4, 12)
        print("v =", v, "|v| =", v:Magnitude())
        v.y = 50
        print("after v.y = 50:", v)

        -- Quaternion just works the same way
        local q1 = Quaternion.new(1, 0, 1, 0)
        local q2 = Quaternion.new(1, 0.5, 0.5, 0.75)
        print("q1 =", q1, " |q1| =", q1:Magnitude())
        print("q1:Conjugate() =", q1:Conjugate())
        print("q1 * q2 =", q1 * q2)

        -- TYPE SAFETY: passing a Quaternion to a Vector3 method is rejected
        -- with a clean Lua error, never a memory-unsafe pointer reinterpretation.
        local ok, err = pcall(function() return v:Dot(q1) end)
        print("v:Dot(q1) ->", ok, err)
    )LUA";

    if (luaL_dostring(L, script) != LUA_OK) {
        std::cerr << "Lua error: " << lua_tostring(L, -1) << '\n';
        lua_pop(L, 1);
    }

    lua_close(L);
    return 0;
}
