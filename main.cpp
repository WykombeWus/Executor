// =============================================================================
//  Embedded Lua in C++ - Minimal Host Example
// -----------------------------------------------------------------------------
//  Demonstrates:
//    1. Host application setup (luaL_newstate / luaL_openlibs)
//    2. C++ -> Lua communication (registering a C++ callback Lua can call)
//    3. Lua script execution (luaL_dostring)
//    4. Lua -> C++ communication (reading a Lua global from C++)
//
//  Build (Lua 5.4 system install):
//      g++ -std=c++17 -Wall -Wextra main.cpp -o executor -llua5.4
//  or just `make` / `cmake -B build && cmake --build build` (see README).
// =============================================================================

#include <cstdio>
#include <iostream>
#include <string>

// Lua is a C library. When pulled into a C++ translation unit we must wrap its
// headers in `extern "C"` so the linker uses C linkage, not C++ name mangling.
extern "C" {
    #include <lua.h>
    #include <lualib.h>
    #include <lauxlib.h>
}

// -----------------------------------------------------------------------------
// 2. C++ -> Lua: a function callable from Lua
// -----------------------------------------------------------------------------
// Every C function exposed to Lua must have the signature:
//     int (*lua_CFunction)(lua_State* L);
//
// Arguments are passed on the Lua stack (1-based indexing):
//     index 1 -> first argument, index 2 -> second argument, ...
// The return value of the C function is the *number of values* the function
// pushed onto the stack as Lua return values.
//
// In Lua, this function will be called as:
//     local ok = my_cpp_function("hello", 42)
// -----------------------------------------------------------------------------
static int my_cpp_function(lua_State* L)
{
    // luaL_check* helpers validate types and raise a Lua error if the caller
    // passed something unexpected. They also pop nothing - they just read.
    const char* msg    = luaL_checkstring(L, 1);   // arg 1: string
    lua_Number  number = luaL_checknumber(L, 2);   // arg 2: number

    std::cout << "[C++] my_cpp_function called from Lua\n"
              << "      message = \"" << msg << "\"\n"
              << "      number  = " << number << "\n";

    // Push a boolean back onto the stack as our return value.
    lua_pushboolean(L, 1);   // 1 == true

    // We pushed exactly one value, so we must return 1.
    return 1;
}

// -----------------------------------------------------------------------------
// Helper: report and clear a Lua error sitting on top of the stack.
// -----------------------------------------------------------------------------
static void report_lua_error(lua_State* L, const char* where)
{
    const char* err = lua_tostring(L, -1);
    std::cerr << "[Lua error in " << where << "] "
              << (err ? err : "(no message)") << '\n';
    lua_pop(L, 1);   // remove the error message from the stack
}

int main()
{
    // -------------------------------------------------------------------------
    // 1. Host application setup
    // -------------------------------------------------------------------------
    // Create a fresh Lua state. This holds *all* Lua data: globals, the stack,
    // registered functions, GC state, etc. Each state is independent.
    lua_State* L = luaL_newstate();
    if (!L) {
        std::cerr << "Failed to create Lua state\n";
        return 1;
    }

    // Open the standard libraries (print, string, math, table, io, os, ...).
    // Without this, even `print` would not exist inside Lua.
    luaL_openlibs(L);

    // -------------------------------------------------------------------------
    // 2. Register the C++ callback as a Lua global named "my_cpp_function"
    // -------------------------------------------------------------------------
    // lua_pushcclosure(L, fn, n) pushes a "C closure": a C function plus n
    // upvalues taken from the top of the stack. With n == 0 it's equivalent to
    // lua_pushcfunction(L, fn) and creates a plain C function value.
    //
    // We then store that value as a global with lua_setglobal, which pops the
    // value off the stack and assigns it to _G["my_cpp_function"].
    lua_pushcclosure(L, my_cpp_function, 0);
    lua_setglobal(L, "my_cpp_function");

    // -------------------------------------------------------------------------
    // 3. Execute a Lua script string
    // -------------------------------------------------------------------------
    // luaL_dostring is shorthand for luaL_loadstring + lua_pcall. It compiles
    // and runs the chunk in protected mode, returning 0 on success or pushing
    // an error message onto the stack on failure.
    //
    // Note: `result` is declared as a *global* (no `local`) so the C++ host
    // can read it back after the script finishes (see step 4).
    const char* script = R"LUA(
        local message = "Hello from Lua"
        local number  = 42
        local success = my_cpp_function(message, number)
        print("[Lua] C++ function returned:", success)

        -- Expose a value back to the host as a global:
        result = "computed by Lua: " .. tostring(success)
    )LUA";

    if (luaL_dostring(L, script) != LUA_OK) {
        report_lua_error(L, "script");
        lua_close(L);
        return 1;
    }

    // -------------------------------------------------------------------------
    // 4. Lua -> C++: read a global the script defined
    // -------------------------------------------------------------------------
    // lua_getglobal pushes _G["result"] onto the stack. We then inspect its
    // type and convert it to a C++ value before popping it off.
    lua_getglobal(L, "result");
    if (lua_isstring(L, -1)) {
        std::string result = lua_tostring(L, -1);
        std::cout << "[C++] read global 'result' from Lua: \""
                  << result << "\"\n";
    } else {
        std::cout << "[C++] global 'result' is not a string (type = "
                  << luaL_typename(L, -1) << ")\n";
    }
    lua_pop(L, 1);   // always balance the stack

    // -------------------------------------------------------------------------
    // Cleanup: lua_close runs all __gc finalizers and frees every byte the
    // Lua state allocated.
    // -------------------------------------------------------------------------
    lua_close(L);
    return 0;
}
