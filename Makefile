CXX ?= g++
# MODE selects the build profile; each lives in its own directory.
#   make                 -> debug-ish dev build   (build/,         -O2)
#   make MODE=release    -> optimized             (build-release/, -O3 -flto)
#   make MODE=profile    -> gprof instrumented    (build-profile/, -O2 -g -pg)
# The engine is integer-only (no floats in state), so -O3/-flto cannot
# change simulation results: state_hash is identical across all modes.
MODE ?= dev
BASEFLAGS := -std=c++20 -Wall -Wextra -Iinclude -Ithird_party/lang-host/include -Ithird_party/lang-host/ggml/include
ifeq ($(MODE),release)
  BUILDDIR := build-release
  CXXFLAGS ?= $(BASEFLAGS) -O3 -flto=auto -march=native -DNDEBUG
  LDFLAGS  ?= -O3 -flto=auto -march=native
else ifeq ($(MODE),profile)
  BUILDDIR := build-profile
  CXXFLAGS ?= $(BASEFLAGS) -O2 -g -pg
  LDFLAGS  ?= -pg
else
  BUILDDIR := build
  CXXFLAGS ?= $(BASEFLAGS) -O2
  LDFLAGS  ?=
endif

SRC := $(wildcard src/*.cpp)
# render CORE (SDL-free): procedural atlas, snapshot, fog — linked into tests
RSRC := $(wildcard src/render/*.cpp)
OBJ := $(SRC:src/%.cpp=$(BUILDDIR)/%.o) $(RSRC:src/render/%.cpp=$(BUILDDIR)/render/%.o)
HDR := $(wildcard include/simlab/*.hpp) $(wildcard include/simlab/render/*.hpp) $(wildcard src/*.hpp)

all: $(BUILDDIR)/tests

$(BUILDDIR)/%.o: src/%.cpp $(HDR)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/render/%.o: src/render/%.cpp $(HDR)
	@mkdir -p $(BUILDDIR)/render
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/test_main.o: tests/test_main.cpp $(HDR)
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/tests: $(OBJ) $(BUILDDIR)/test_main.o
	$(CXX) $(LDFLAGS) $^ -ldl -o $@

test: $(BUILDDIR)/tests
	./$(BUILDDIR)/tests

# ---- Phase 13: sunnyworld_viewer (SDL3 + Dear ImGui, read-only observer) ------
# SDL3 is vendored in third_party/SDL-main and built static once (X11 is
# loaded at runtime; compile-time X11 headers live in third_party/x11-headers).
# The tests binary never links SDL: the render CORE (atlas/snapshot/fog) is
# SDL-free and is what the tests exercise.
SDL_DIR   := third_party/SDL-main
SDL_BUILD := third_party/sdl3-build
SDL_LIB   := $(SDL_BUILD)/libSDL3.a
IMGUI_DIR := third_party/imgui
IMGUI_SRC := $(IMGUI_DIR)/imgui.cpp $(IMGUI_DIR)/imgui_draw.cpp $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp $(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_sdlrenderer3.cpp
IMGUI_OBJ := $(IMGUI_SRC:third_party/imgui/%.cpp=$(BUILDDIR)/imgui/%.o)
VIEW_FLAGS := -I$(SDL_DIR)/include -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends

# x11-headers/include holds the X11 dev headers (extracted from the distro -dev
# .debs, no sudo needed); lib/ gets symlinks to the RUNTIME .so files already
# on the machine so FindX11 is satisfied - SDL loads them with dlopen.
X11_LIBS := X11 Xext Xcursor Xi Xfixes Xrandr Xrender Xss xkbcommon
$(SDL_LIB):
	@mkdir -p third_party/x11-headers/lib
	@for l in $(X11_LIBS); do f=$$(ls /usr/lib/x86_64-linux-gnu/lib$$l.so.* 2>/dev/null | head -1); \
	  [ -n "$$f" ] && ln -sf "$$f" third_party/x11-headers/lib/lib$$l.so; done; true
	cmake -S $(SDL_DIR) -B $(SDL_BUILD) -DCMAKE_BUILD_TYPE=Release \
	  -DCMAKE_PREFIX_PATH=$(CURDIR)/third_party/x11-headers \
	  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF \
	  -DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_X11_XTEST=OFF -DSDL_X11_XSYNC=OFF \
	  -DSDL_X11_XDBE=OFF -DSDL_X11_XSHAPE=OFF -DSDL_WAYLAND=OFF -DSDL_AUDIO=OFF \
	  -DSDL_CAMERA=OFF -DSDL_HIDAPI=OFF -DSDL_VULKAN=OFF -DSDL_RENDER_VULKAN=OFF \
	  -DSDL_GPU=OFF -DSDL_RENDER_GPU=OFF
	cmake --build $(SDL_BUILD) -j8

$(BUILDDIR)/imgui/%.o: third_party/imgui/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 -O2 $(VIEW_FLAGS) -c $< -o $@

VIEWER_SRC := src/render/sdl/viewer_main.cpp src/render/sdl/viewer_core.cpp \
              src/render/sdl/viewer_planet.cpp src/render/sdl/viewer_entities.cpp \
              src/render/sdl/viewer_hud.cpp src/render/sdl/viewer_tools.cpp \
              src/render/sdl/viewer_mind.cpp src/render/sdl/viewer3d.cpp
VIEWER_OBJ := $(VIEWER_SRC:src/render/sdl/%.cpp=$(BUILDDIR)/viewer/%.o)

$(BUILDDIR)/viewer/%.o: src/render/sdl/%.cpp src/render/sdl/viewer.hpp $(HDR) $(SDL_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(VIEW_FLAGS) -c $< -o $@

$(BUILDDIR)/sunnyworld_viewer: $(OBJ) $(IMGUI_OBJ) $(VIEWER_OBJ) $(SDL_LIB)
	$(CXX) $(LDFLAGS) $(OBJ) $(IMGUI_OBJ) $(VIEWER_OBJ) $(SDL_LIB) -ldl -lpthread -lm -o $@

viewer sunnyworld_viewer: $(BUILDDIR)/sunnyworld_viewer

sunnyworld_tests: $(BUILDDIR)/tests
	@ln -sf tests $(BUILDDIR)/sunnyworld_tests

release:
	$(MAKE) MODE=release

profile:
	$(MAKE) MODE=profile

clean:
	rm -rf build build-release build-profile

.PHONY: all test clean release profile viewer sunnyworld_viewer sunnyworld_tests
