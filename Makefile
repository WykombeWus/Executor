# Simple Makefile for the embedded-Lua example.
# Tries pkg-config first, falls back to -llua5.4, then -llua.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

LUA_PKG  := $(shell pkg-config --exists lua5.4 && echo lua5.4 || \
                    (pkg-config --exists lua   && echo lua))
ifneq ($(LUA_PKG),)
    LUA_CFLAGS := $(shell pkg-config --cflags $(LUA_PKG))
    LUA_LIBS   := $(shell pkg-config --libs   $(LUA_PKG))
else
    LUA_CFLAGS :=
    LUA_LIBS   := -llua5.4
endif

all: executor vector3 userdata_template class_builder

executor: main.cpp
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) main.cpp -o $@ $(LUA_LIBS)

vector3: vector3.cpp
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) vector3.cpp -o $@ $(LUA_LIBS)

userdata_template: userdata_template.cpp
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) userdata_template.cpp -o $@ $(LUA_LIBS)

class_builder: class_builder.cpp
	$(CXX) $(CXXFLAGS) $(LUA_CFLAGS) class_builder.cpp -o $@ $(LUA_LIBS)

run: executor
	./executor

run-vector3: vector3
	./vector3

run-userdata: userdata_template
	./userdata_template

run-builder: class_builder
	./class_builder

clean:
	rm -f executor vector3 userdata_template class_builder

.PHONY: all run run-vector3 run-userdata run-builder clean
