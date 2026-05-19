cmake_minimum_required(VERSION 3.15)
project(executor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CMake ships a FindLua module that locates Lua 5.x headers and libraries.
find_package(Lua REQUIRED)

add_executable(executor main.cpp)
target_include_directories(executor PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(executor PRIVATE ${LUA_LIBRARIES})

add_executable(vector3 vector3.cpp)
target_include_directories(vector3 PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(vector3 PRIVATE ${LUA_LIBRARIES})

add_executable(userdata_template userdata_template.cpp)
target_include_directories(userdata_template PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(userdata_template PRIVATE ${LUA_LIBRARIES})

foreach(target executor vector3 userdata_template)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
