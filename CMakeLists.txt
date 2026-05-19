cmake_minimum_required(VERSION 3.15)
project(executor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake ships a FindLua module that locates Lua 5.x headers and libraries.
find_package(Lua REQUIRED)

add_executable(executor main.cpp)
target_include_directories(executor PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(executor PRIVATE ${LUA_LIBRARIES})

if(MSVC)
    target_compile_options(executor PRIVATE /W4)
else()
    target_compile_options(executor PRIVATE -Wall -Wextra)
endif()
