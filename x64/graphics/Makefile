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

# O3: Vulkan headers already vendored by this repository; direct ICD loading.
all: out/vulkan_clear.exe
out/vulkan_clear.exe: vulkan_clear.c | out
	x86_64-w64-mingw32-gcc -O2 -std=c11 -Wall -Wextra -Werror -I../../source/vulkan/vk $< -o $@

# O4: standalone on-device D3D12 probe (Windows target; d3d12/dxgi loaded at runtime).
all: out/probe_d3d12.exe
out/probe_d3d12.exe: probe_d3d12.c | out
	x86_64-w64-mingw32-gcc -O2 -std=c11 -Wall -Wextra -Werror -municode -static-libgcc -s $< -o $@ -ldxguid
