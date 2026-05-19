// =============================================================================
//  Exposing a C++ class to Lua: Vector3
// -----------------------------------------------------------------------------
//  Demonstrates the canonical "userdata + metatable" binding pattern:
//
//    * Full userdata holds the C++ object's bytes; Lua manages its lifetime.
//    * A metatable named "Vector3" is registered once with luaL_newmetatable.
//    * __index   handles property reads  (v.x)         and method dispatch (v:length())
//    * __newindex handles property writes (v.y = 50)
//    * __gc      runs the C++ destructor when Lua collects the object
//    * __tostring gives `print(v)` a nice format
//    * __add     overloads the + operator (v + w)
//
//  Lua usage (the script we run at the bottom of main):
//      local v = Vector3.new(10, 20, 30)
//      print(v.x)           --> 10
//      v.y = 50
//      print(v.y)           --> 50
//      print(v:length())    --> sqrt(10*10 + 50*50 + 30*30)
//      print(v + Vector3.new(1, 2, 3))
//
//  Build:  g++ -std=c++17 -Wall -Wextra vector3.cpp -o vector3 -llua -lm -ldl
// =============================================================================

#include <cmath>
#include <cstring>
#include <iostream>
#include <new>          // placement new

extern "C" {
    #include <lua.h>
    #include <lualib.h>
    #include <lauxlib.h>
}

// -----------------------------------------------------------------------------
// The C++ class we want to expose. Kept simple for clarity, but it could be
// any class with constructors, virtual functions, etc.
// -----------------------------------------------------------------------------
struct Vector3 {
    double x, y, z;

    Vector3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    double length() const { return std::sqrt(x * x + y * y + z * z); }
};

// A unique string used as the metatable's name in the Lua registry.
// Using a constant avoids typos and is the standard idiom.
static const char* VECTOR3_MT = "Vector3";

// -----------------------------------------------------------------------------
// Helper: validate that stack slot `idx` is a Vector3 userdata and return a
// typed pointer. luaL_checkudata raises a Lua error if the metatable doesn't
// match - this is what stops a Lua script from passing some other userdata
// (or a number, or nil) to a Vector3 method.
// -----------------------------------------------------------------------------
static Vector3* check_vector3(lua_State* L, int idx)
{
    void* ud = luaL_checkudata(L, idx, VECTOR3_MT);
    return static_cast<Vector3*>(ud);
}

// Helper that constructs a fresh Vector3 userdata on top of the stack and
// attaches the metatable. Used by Vector3.new and operator+.
static Vector3* push_new_vector3(lua_State* L, double x, double y, double z)
{
    // lua_newuserdata allocates Lua-managed memory of the requested size and
    // pushes a userdata value referencing it. We then placement-new our C++
    // object into that memory so its constructor actually runs.
    void* mem = lua_newuserdata(L, sizeof(Vector3));
    Vector3* v = new (mem) Vector3(x, y, z);

    // Attach the metatable so Lua knows this userdata's type.
    luaL_getmetatable(L, VECTOR3_MT);
    lua_setmetatable(L, -2);     // pops the metatable, leaves userdata on top
    return v;
}

// -----------------------------------------------------------------------------
// Vector3.new(x, y, z) -- the constructor exposed to Lua
// -----------------------------------------------------------------------------
static int vector3_new(lua_State* L)
{
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double z = luaL_checknumber(L, 3);
    push_new_vector3(L, x, y, z);
    return 1;   // we pushed one value (the userdata)
}

// -----------------------------------------------------------------------------
// __gc -- runs when Lua's garbage collector reclaims the userdata.
// We must invoke the destructor manually because we used placement new.
// -----------------------------------------------------------------------------
static int vector3_gc(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    v->~Vector3();
    return 0;
}

// -----------------------------------------------------------------------------
// __tostring -- called by `print(v)` and `tostring(v)`.
// -----------------------------------------------------------------------------
static int vector3_tostring(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    lua_pushfstring(L, "Vector3(%f, %f, %f)", v->x, v->y, v->z);
    return 1;
}

// -----------------------------------------------------------------------------
// __add -- called for `a + b` when either operand has this metamethod.
// -----------------------------------------------------------------------------
static int vector3_add(lua_State* L)
{
    Vector3* a = check_vector3(L, 1);
    Vector3* b = check_vector3(L, 2);
    push_new_vector3(L, a->x + b->x, a->y + b->y, a->z + b->z);
    return 1;
}

// -----------------------------------------------------------------------------
// Method: v:length()
// In Lua, `v:length()` desugars to `v.length(v)`, so the userdata is arg 1.
// -----------------------------------------------------------------------------
static int vector3_length(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    lua_pushnumber(L, v->length());
    return 1;
}

// -----------------------------------------------------------------------------
// __index handler.
//
// In Lua, `v.x` triggers __index with key = "x". We split behavior:
//   * If the key is a known field name (x/y/z), return the field value.
//   * Otherwise look the key up in a "methods" table (upvalue 1) so that
//     `v:length()` resolves to the C function we registered.
//
// Stashing the methods table as an upvalue of the closure is cleaner than
// a global lookup and avoids polluting the metatable itself.
// -----------------------------------------------------------------------------
static int vector3_index(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    const char* key = luaL_checkstring(L, 2);

    if (std::strcmp(key, "x") == 0) { lua_pushnumber(L, v->x); return 1; }
    if (std::strcmp(key, "y") == 0) { lua_pushnumber(L, v->y); return 1; }
    if (std::strcmp(key, "z") == 0) { lua_pushnumber(L, v->z); return 1; }

    // Method lookup: methods_table[key]
    lua_pushvalue(L, lua_upvalueindex(1));   // push methods table
    lua_pushvalue(L, 2);                     // push the key
    lua_rawget(L, -2);                       // methods[key], leaves nil if absent
    return 1;
}

// -----------------------------------------------------------------------------
// __newindex handler -- assignments like `v.y = 50`.
// -----------------------------------------------------------------------------
static int vector3_newindex(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    const char* key = luaL_checkstring(L, 2);
    double val = luaL_checknumber(L, 3);

    if (std::strcmp(key, "x") == 0) { v->x = val; return 0; }
    if (std::strcmp(key, "y") == 0) { v->y = val; return 0; }
    if (std::strcmp(key, "z") == 0) { v->z = val; return 0; }

    // Reject unknown fields with a Lua error rather than silently allowing them.
    return luaL_error(L, "Vector3 has no field '%s'", key);
}

// -----------------------------------------------------------------------------
// Register the Vector3 type and its constructor table with the Lua state.
// Call this once, right after luaL_openlibs.
// -----------------------------------------------------------------------------
static void register_vector3(lua_State* L)
{
    // ---- 1. Build the metatable -------------------------------------------
    // luaL_newmetatable creates a fresh table in the registry under the given
    // name and pushes it. If a metatable with this name already exists it
    // pushes the existing one and returns 0 (useful for idempotent setup).
    luaL_newmetatable(L, VECTOR3_MT);          // stack: [mt]

    // ---- 2. Methods table (for v:length(), v:dot(), etc.) -----------------
    lua_newtable(L);                           // stack: [mt, methods]
    lua_pushcfunction(L, vector3_length);
    lua_setfield(L, -2, "length");             // methods.length = vector3_length

    // __index = closure(vector3_index) with `methods` captured as upvalue 1.
    // After lua_pushcclosure the methods table is consumed and replaced by
    // the closure value on the stack.
    lua_pushcclosure(L, vector3_index, 1);     // stack: [mt, __index_closure]
    lua_setfield(L, -2, "__index");            // mt.__index = closure

    // ---- 3. The remaining metamethods -------------------------------------
    lua_pushcfunction(L, vector3_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, vector3_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, vector3_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, vector3_add);
    lua_setfield(L, -2, "__add");

    lua_pop(L, 1);                             // stack: []  (drop metatable)

    // ---- 4. Global "Vector3" table containing the constructor -------------
    // After this, Lua code can call Vector3.new(x, y, z).
    lua_newtable(L);                           // stack: [Vector3]
    lua_pushcfunction(L, vector3_new);
    lua_setfield(L, -2, "new");                // Vector3.new = vector3_new
    lua_setglobal(L, "Vector3");               // _G.Vector3 = table
}

// -----------------------------------------------------------------------------
// Driver: open Lua, register the binding, run a demo script.
// -----------------------------------------------------------------------------
int main()
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    register_vector3(L);

    static const char* script = R"LUA(
        local v = Vector3.new(10, 20, 30)
        print("v.x =", v.x)        -- 10
        print("v.y =", v.y)        -- 20
        print("v.z =", v.z)        -- 30

        v.y = 50
        print("after v.y = 50 ->", v.y)

        print("tostring(v) =", tostring(v))
        print("v:length() =",   v:length())

        local w = Vector3.new(1, 2, 3)
        local sum = v + w
        print("v + w =",        sum)
        print("(v+w).x =",      sum.x)

        -- Type safety: this raises a Lua error caught by pcall.
        local ok, err = pcall(function() v.bogus = 1 end)
        print("setting unknown field -> ok =", ok, " err =", err)
    )LUA";

    if (luaL_dostring(L, script) != LUA_OK) {
        std::cerr << "Lua error: " << lua_tostring(L, -1) << '\n';
        lua_pop(L, 1);
    }

    lua_close(L);
    return 0;
}
