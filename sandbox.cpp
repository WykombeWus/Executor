// =============================================================================
//  Sandboxed Lua environment for running untrusted plugin scripts.
// -----------------------------------------------------------------------------
//  Threat model: scripts come from an unknown source. They must NOT be able to
//    * read or write the file system     (no io, no os.remove/rename/tmpname)
//    * spawn processes                   (no os.execute, no os.exit)
//    * load other code                   (no dofile, loadfile, load, require)
//    * inspect or rewrite host internals (no debug library, no metatable poke)
//    * exhaust CPU                       (instruction-count hook trips an error)
//    * break out via _G or _ENV          (each script runs with a custom _ENV
//                                         that contains only what we placed in)
//
//  Strategy:
//    1. Build a fresh lua_State.
//    2. Open the standard libraries we need into the registry (so their tables
//       exist in _G), but never expose _G to scripts.
//    3. Hand-pick whitelisted functions / sub-tables into a sandbox env table.
//    4. Add the host's custom API (Vector3) into the sandbox env.
//    5. When loading a script, set its first upvalue (_ENV in 5.2+) to the
//       sandbox env. All global lookups in the script then resolve through
//       this restricted table - never through _G.
//    6. Optionally install a debug hook that fires every N instructions and
//       raises an error, capping CPU per script invocation.
//
//  Build:  g++ -std=c++17 -Wall -Wextra sandbox.cpp -o sandbox -llua -lm -ldl
// =============================================================================

#include "lua_binding.hpp"

#include <cmath>
#include <iostream>
#include <string>

// =============================================================================
//  Vector3 -- the host's custom API, exposed through the sandbox.
// =============================================================================
struct Vector3 {
    double x, y, z;
    Vector3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    double magnitude() const { return std::sqrt(x*x + y*y + z*z); }
    double dot(const Vector3& o) const { return x*o.x + y*o.y + z*o.z; }
    Vector3 add(const Vector3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    std::string toString() const {
        return "Vector3(" + std::to_string(x) + ", "
                          + std::to_string(y) + ", "
                          + std::to_string(z) + ")";
    }
};

// =============================================================================
//  LuaSandbox
// =============================================================================
class LuaSandbox {
    lua_State* L_;
    int env_ref_;   // luaL_ref handle to the sandbox env table

    // Hook used to enforce the per-script instruction budget.
    static void instruction_hook(lua_State* L, lua_Debug* /*ar*/) {
        // Raises a Lua error. lua_pcall in run() will catch it.
        luaL_error(L, "script exceeded instruction limit");
    }

public:
    LuaSandbox() {
        L_ = luaL_newstate();

        // ---- Open the libs whose contents we want available, into _G ----
        // luaL_requiref(L, name, openf, glb=1) calls openf, sets _G[name] to
        // the result, and leaves the result on top of the stack. We pop it
        // because we'll re-fetch via lua_getglobal when copying selectively.
        struct LibSpec { const char* name; lua_CFunction openf; };
        const LibSpec libs[] = {
            {"_G",            luaopen_base},
            {LUA_STRLIBNAME,  luaopen_string},
            {LUA_MATHLIBNAME, luaopen_math},
            {LUA_TABLIBNAME,  luaopen_table},
            {LUA_OSLIBNAME,   luaopen_os},
            // NOT loaded: io, debug, package, coroutine.
        };
        for (auto& l : libs) {
            luaL_requiref(L_, l.name, l.openf, 1);
            lua_pop(L_, 1);
        }

        // ---- Build the sandbox env table by hand ----
        lua_newtable(L_);                             // [env]
        const int env = lua_gettop(L_);

        // Whitelisted functions from the base library.
        // Notably ABSENT: dofile, loadfile, load, loadstring, require,
        //                 rawget, rawset, rawlen, rawequal,
        //                 setmetatable, getmetatable, collectgarbage,
        //                 _G, _ENV.  The first row would let scripts run
        //                 arbitrary code; the second would let them bypass
        //                 metatables; the third would let them DoS or escape.
        static const char* base_safe[] = {
            "assert", "error", "ipairs", "next", "pairs",
            "pcall", "xpcall", "print", "select",
            "tonumber", "tostring", "type", "_VERSION",
            nullptr
        };
        for (int i = 0; base_safe[i]; ++i) {
            lua_getglobal(L_, base_safe[i]);          // [env, fn]
            lua_setfield(L_, env, base_safe[i]);      // env[name] = fn
        }

        // Whole-library copies (every function in these libs is safe).
        // NOTE: this aliases the same tables as _G.string / _G.math / _G.table.
        // A defense-in-depth implementation would wrap them in read-only
        // proxies so a script can't pollute math.* for the next script.
        for (const char* lib : {"string", "math", "table"}) {
            lua_getglobal(L_, lib);                   // [env, lib]
            lua_setfield(L_, env, lib);               // env[lib] = lib
        }

        // Restricted "os" - only the read-only, side-effect-free pieces.
        // Specifically excluded: execute, exit, remove, rename, tmpname,
        //                        getenv, setlocale.
        lua_newtable(L_);                             // [env, restricted_os]
        lua_getglobal(L_, "os");                      // [env, restricted_os, os]
        for (const char* fn : {"clock", "date", "difftime", "time"}) {
            lua_getfield(L_, -1, fn);                 // [env, ros, os, fn]
            lua_setfield(L_, -3, fn);                 // ros[name] = fn
        }
        lua_pop(L_, 1);                               // [env, restricted_os]
        lua_setfield(L_, env, "os");                  // env.os = restricted_os

        // ---- Anchor the env table in the registry so we can fetch it later ----
        env_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);   // pops env
    }

    ~LuaSandbox() {
        if (L_) lua_close(L_);
    }

    LuaSandbox(const LuaSandbox&) = delete;
    LuaSandbox& operator=(const LuaSandbox&) = delete;

    lua_State* state() { return L_; }

    // Push the sandbox env onto the stack.
    void push_env() {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, env_ref_);
    }

    // Move a global created by Class<T> registration into the sandbox env,
    // and remove it from _G to avoid leaking the binding back to host code
    // that might re-use this lua_State for non-sandbox purposes.
    void expose_global(const char* name) {
        push_env();                                   // [env]
        lua_getglobal(L_, name);                      // [env, value]
        lua_setfield(L_, -2, name);                   // env[name] = value
        lua_pop(L_, 1);                               // []
        lua_pushnil(L_);
        lua_setglobal(L_, name);                      // _G[name] = nil
    }

    // Compile and execute a script in the sandbox.
    // If instruction_limit > 0, install a count hook that aborts after that
    // many VM instructions (a coarse CPU cap, not a real-time deadline).
    bool run(const char* script, std::string& err, int instruction_limit = 0) {
        // Compile.
        if (luaL_loadstring(L_, script) != LUA_OK) {
            err = lua_tostring(L_, -1);
            lua_pop(L_, 1);
            return false;
        }
        // Stack: [chunk]

        // Override the chunk's _ENV upvalue with our sandbox env table.
        // In Lua 5.2+, every chunk has _ENV as its first upvalue, and all
        // global accesses inside the chunk are sugar for `_ENV[name]`.
        push_env();                                   // [chunk, env]
        if (lua_setupvalue(L_, -2, 1) == nullptr) {
            err = "could not set _ENV upvalue";
            lua_pop(L_, 2);
            return false;
        }
        // Stack: [chunk]

        // CPU cap, if requested.
        if (instruction_limit > 0) {
            lua_sethook(L_, &instruction_hook, LUA_MASKCOUNT, instruction_limit);
        }

        int rc = lua_pcall(L_, 0, 0, 0);

        if (instruction_limit > 0) {
            lua_sethook(L_, nullptr, 0, 0);
        }

        if (rc != LUA_OK) {
            err = lua_tostring(L_, -1);
            lua_pop(L_, 1);
            return false;
        }
        return true;
    }
};

// =============================================================================
//  Driver
// =============================================================================
static void test(LuaSandbox& sb, const char* label, const char* script,
                 int instruction_limit = 0)
{
    std::cout << "\n--- " << label << " ---\n";
    std::string err;
    bool ok = sb.run(script, err, instruction_limit);
    if (!ok) {
        std::cout << "[blocked] " << err << "\n";
    } else {
        std::cout << "[ok]\n";
    }
}

int main() {
    LuaSandbox sb;

    // Bind Vector3 using the same Class<T> DSL as before. The binding
    // initially lands in _G (because Class<T>::end() does lua_setglobal),
    // and then expose_global moves it into the sandbox env.
    Class<Vector3>(sb.state(), "Vector3")
        .constructor<double, double, double>()
        .property("x", &Vector3::x)
        .property("y", &Vector3::y)
        .property("z", &Vector3::z)
        .method  ("Magnitude",  &Vector3::magnitude)
        .method  ("Dot",        &Vector3::dot)
        .meta    ("__add",      &Vector3::add)
        .meta    ("__tostring", &Vector3::toString)
        .end();

    sb.expose_global("Vector3");

    // ---- 1. A well-behaved script ----
    test(sb, "well-behaved script", R"LUA(
        local v = Vector3.new(3, 4, 12)
        print("|v| =", v:Magnitude())
        print("greeting =", string.upper("hello"))
        print("pi =", math.pi)
        print("clock =", os.clock(), "(safe os subset works)")
    )LUA");

    // ---- 2. Tries to spawn a process ----
    test(sb, "os.execute is removed", R"LUA(
        os.execute("rm -rf /")
    )LUA");

    // ---- 3. Tries to read a file ----
    test(sb, "io library is absent", R"LUA(
        local f = io.open("/etc/passwd", "r")
        print(f:read("*all"))
    )LUA");

    // ---- 4. Tries to load arbitrary code ----
    test(sb, "dofile is removed", R"LUA(
        dofile("/etc/passwd")
    )LUA");

    test(sb, "load / loadstring are removed", R"LUA(
        local f = load("return os.execute('echo pwned')")
        f()
    )LUA");

    // ---- 5. Tries to inspect host internals via debug ----
    test(sb, "debug library is absent", R"LUA(
        debug.getregistry()
    )LUA");

    // ---- 6. Tries to escape via metatable manipulation ----
    test(sb, "setmetatable is removed", R"LUA(
        local v = Vector3.new(1, 2, 3)
        setmetatable(v, { __index = function() return "owned" end })
    )LUA");

    // ---- 7. Tries to reach the real _G ----
    test(sb, "_G is not in the sandbox env", R"LUA(
        print(_G.os.execute)
    )LUA");

    // ---- 8. CPU cap: an infinite loop ----
    test(sb, "infinite loop tripped by instruction hook",
         "while true do end",
         /*instruction_limit=*/100000);

    // ---- 9. Sanity: well-behaved script still works after blocked attempts ----
    test(sb, "sandbox is reusable after blocked attempts", R"LUA(
        print("sandbox still works:", Vector3.new(1, 2, 2):Magnitude())  -- 3
    )LUA");

    return 0;
}
