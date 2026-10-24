CXX_WIN ?= x86_64-w64-mingw32-g++
FLAGS := -O2 -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -static-libgcc -static-libstdc++
all: out/dcomp.dll out/test_dcomp.exe
out:
	mkdir -p $@
out/dcomp.dll: dcomp_stub.cpp | out
	$(CXX_WIN) $(FLAGS) -shared $< -o $@ -luuid
out/test_dcomp.exe: tests/test_dcomp.cpp | out
	$(CXX_WIN) $(FLAGS) $< -o $@ -luuid
.PHONY: all
