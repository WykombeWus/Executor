// =============================================================================
//  Exposing a C++ class to Lua: Vector3 (production-style binding)
// -----------------------------------------------------------------------------
//  Same behavior as the previous example, refactored to the idiomatic
//  table-driven registration pattern used in production Lua bindings:
//
//    * `luaL_Reg` arrays declare methods and metamethods as data, not code.
//    * `luaL_setfuncs` walks an array and registers every entry into the table
//      currently on top of the stack, optionally with shared upvalues.
//    * `luaL_newlib`  builds the module table for the constructor.
//
//  Trick that makes this clean: every metamethod is registered with the
//  *methods* table as a shared upvalue. Only `__index` actually consults it
//  (for `v:Magnitude()`-style dispatch); the others ignore the upvalue.
//  This keeps everything declarative without losing property access (v.x).
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
// The C++ class we're binding.
// -----------------------------------------------------------------------------
struct Vector3 {
    double x, y, z;

    Vector3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    double magnitude() const { return std::sqrt(x*x + y*y + z*z); }
    double dot(const Vector3& o) const { return x*o.x + y*o.y + z*o.z; }
};

// Registry key for the metatable. Constants beat scattered string literals.
static const char* VECTOR3_MT = "Vector3";

// -----------------------------------------------------------------------------
// Stack helpers
// -----------------------------------------------------------------------------
static Vector3* check_vector3(lua_State* L, int idx)
{
    return static_cast<Vector3*>(luaL_checkudata(L, idx, VECTOR3_MT));
}

// Construct a fresh Vector3 userdata on top of the stack.
static Vector3* push_new_vector3(lua_State* L, double x, double y, double z)
{
    void* mem = lua_newuserdata(L, sizeof(Vector3));
    Vector3* v = new (mem) Vector3(x, y, z);
    luaL_getmetatable(L, VECTOR3_MT);
    lua_setmetatable(L, -2);
    return v;
}

// =============================================================================
// Methods - exposed as v:Method(...) in Lua
// =============================================================================

static int vector3_magnitude(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    lua_pushnumber(L, v->magnitude());
    return 1;
}

static int vector3_dot(lua_State* L)
{
    Vector3* a = check_vector3(L, 1);
    Vector3* b = check_vector3(L, 2);
    lua_pushnumber(L, a->dot(*b));
    return 1;
}

static int vector3_normalize(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    double m = v->magnitude();
    if (m == 0.0) {
        return luaL_error(L, "cannot normalize a zero-length vector");
    }
    push_new_vector3(L, v->x / m, v->y / m, v->z / m);
    return 1;
}

static int vector3_scale(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    double s = luaL_checknumber(L, 2);
    push_new_vector3(L, v->x * s, v->y * s, v->z * s);
    return 1;
}

// =============================================================================
// Metamethods
// =============================================================================

// __index: properties first, then fall back to the methods table.
// The methods table is shared upvalue 1 (set up by luaL_setfuncs in
// register_vector3 below).
static int vector3_index(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    const char* key = luaL_checkstring(L, 2);

    if (std::strcmp(key, "x") == 0) { lua_pushnumber(L, v->x); return 1; }
    if (std::strcmp(key, "y") == 0) { lua_pushnumber(L, v->y); return 1; }
    if (std::strcmp(key, "z") == 0) { lua_pushnumber(L, v->z); return 1; }

    // methods[key] - returns nil if no such method, which Lua will surface
    // as a normal "attempt to call a nil value" if the user mistypes.
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    return 1;
}

static int vector3_newindex(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    const char* key = luaL_checkstring(L, 2);
    double val = luaL_checknumber(L, 3);

    if (std::strcmp(key, "x") == 0) { v->x = val; return 0; }
    if (std::strcmp(key, "y") == 0) { v->y = val; return 0; }
    if (std::strcmp(key, "z") == 0) { v->z = val; return 0; }

    return luaL_error(L, "Vector3 has no field '%s'", key);
}

static int vector3_gc(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    v->~Vector3();
    return 0;
}

static int vector3_tostring(lua_State* L)
{
    Vector3* v = check_vector3(L, 1);
    lua_pushfstring(L, "Vector3(%f, %f, %f)", v->x, v->y, v->z);
    return 1;
}

static int vector3_add(lua_State* L)
{
    Vector3* a = check_vector3(L, 1);
    Vector3* b = check_vector3(L, 2);
    push_new_vector3(L, a->x + b->x, a->y + b->y, a->z + b->z);
    return 1;
}

static int vector3_sub(lua_State* L)
{
    Vector3* a = check_vector3(L, 1);
    Vector3* b = check_vector3(L, 2);
    push_new_vector3(L, a->x - b->x, a->y - b->y, a->z - b->z);
    return 1;
}

static int vector3_eq(lua_State* L)
{
    Vector3* a = check_vector3(L, 1);
    Vector3* b = check_vector3(L, 2);
    lua_pushboolean(L, a->x == b->x && a->y == b->y && a->z == b->z);
    return 1;
}

// =============================================================================
// Module-level constructor: Vector3.new(x, y, z)
// =============================================================================
static int vector3_new(lua_State* L)
{
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);
    double z = luaL_checknumber(L, 3);
    push_new_vector3(L, x, y, z);
    return 1;
}

// =============================================================================
// Registration arrays - this is the heart of the production-style pattern.
// Each entry is { lua_name, c_function }. The list MUST end with {NULL, NULL}
// so luaL_setfuncs / luaL_newlib know where it stops.
// =============================================================================

static const luaL_Reg vector3_methods[] = {
    {"Magnitude", vector3_magnitude},
    {"Dot",       vector3_dot},
    {"Normalize", vector3_normalize},
    {"Scale",     vector3_scale},
    {nullptr,     nullptr}
};

static const luaL_Reg vector3_metamethods[] = {
    {"__index",    vector3_index},
    {"__newindex", vector3_newindex},
    {"__gc",       vector3_gc},
    {"__tostring", vector3_tostring},
    {"__add",      vector3_add},
    {"__sub",      vector3_sub},
    {"__eq",       vector3_eq},
    {nullptr,      nullptr}
};

// Module table - the value bound to the global name "Vector3".
static const luaL_Reg vector3_module[] = {
    {"new", vector3_new},
    {nullptr, nullptr}
};

// -----------------------------------------------------------------------------
// register_vector3 - call once, after luaL_openlibs.
//
// Stack discipline is the whole story here. Each annotated step shows the
// stack from bottom (left) to top (right); [] means empty.
// -----------------------------------------------------------------------------
static void register_vector3(lua_State* L)
{
    // 1. Create (or fetch) the metatable in the registry.
    luaL_newmetatable(L, VECTOR3_MT);              // [mt]

    // 2. Build the methods table and populate it via luaL_setfuncs.
    //    luaL_setfuncs(L, regs, nup) registers every entry of `regs` into
    //    the table on top of the stack, sharing `nup` upvalues among them.
    //    With nup == 0 it's a plain bulk lua_pushcfunction + lua_setfield.
    lua_newtable(L);                               // [mt, methods]
    luaL_setfuncs(L, vector3_methods, 0);          // [mt, methods]

    // 3. Register the metamethods into `mt`, with `methods` as a shared
    //    upvalue (so __index can find it via lua_upvalueindex(1)).
    //
    //    luaL_setfuncs expects:
    //      stack: [..., target_table, upvalue_1, ..., upvalue_N]
    //    It pops the upvalues after registration, leaving target_table.
    //
    //    Right now the stack is [mt, methods], so `mt` is the target and
    //    `methods` is the single upvalue - exactly what we want.
    luaL_setfuncs(L, vector3_metamethods, 1);      // [mt]   (methods popped)

    lua_pop(L, 1);                                 // []     (drop mt)

    // 4. Build the module table {new = vector3_new} and bind it to _G.Vector3.
    //    luaL_newlib creates a fresh table sized for the array and fills it.
    luaL_newlib(L, vector3_module);                // [Vector3]
    lua_setglobal(L, "Vector3");                   // []
}

// -----------------------------------------------------------------------------
// Demo
// -----------------------------------------------------------------------------
int main()
{
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    register_vector3(L);

    static const char* script = R"LUA(
        local v = Vector3.new(3, 4, 12)
        print("v.x, v.y, v.z =", v.x, v.y, v.z)
        print("v:Magnitude() =", v:Magnitude())   -- sqrt(9+16+144) = 13

        v.y = 50
        print("after v.y = 50 ->", v.y)

        local w = Vector3.new(1, 0, 0)
        print("v:Dot(w) =", v:Dot(w))             -- 3

        local n = Vector3.new(3, 0, 4):Normalize()
        print("Normalize(3,0,4) =", n, " mag=", n:Magnitude())

        print("v + w =",  v + w)
        print("v - w =",  v - w)
        print("v == Vector3.new(3, 50, 12) =",
              v == Vector3.new(3, 50, 12))

        print("v:Scale(2) =", v:Scale(2))

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
