# Ferrari Virtual Race - native macOS x86_64 build of the statically recompiled game.
CC      = clang
CXX     = clang++
PYTHON ?= .venv/bin/python
EXE    ?= ../extracted/FerrariVR_Hi.exe
ARCH    = -arch x86_64 -mmacosx-version-min=26.0
CFLAGS  = $(ARCH) -O2 -w -Iruntime -fno-strict-aliasing
CXXFLAGS= $(ARCH) -O1 -g -std=c++17 -Iruntime -Wall -Wno-unused-function -Wno-unused-variable -fno-strict-aliasing
GEN_SRC = $(wildcard gen/*.c)
GEN_OBJ = $(patsubst gen/%.c,build/obj/%.o,$(GEN_SRC))
RT_SRC  = $(wildcard runtime/*.cpp)
RT_OBJ  = $(patsubst runtime/%.cpp,build/rt/%.o,$(RT_SRC)) build/rt/rt_metal.o
LIBS    = $(shell pkg-config --libs sdl3 2>/dev/null) -framework OpenGL -framework Cocoa -framework ImageIO -framework CoreGraphics -framework Metal -framework IOSurface -framework Foundation

build/fvr: $(GEN_OBJ) $(RT_OBJ)
	$(CXX) $(ARCH) -o $@ $^ $(LIBS)

# tinybvh (third_party/tiny_bvh.h) is compiled into rt_scene.o: optimise it
build/rt/rt_scene.o: CXXFLAGS += -O3 -mavx2 -mfma -Wno-unused-but-set-variable -Wno-unused-parameter

build/shadertest: tests/shadertest.cpp build/rt/d3d9_shader.o build/rt/pbr.o
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags sdl3 2>/dev/null) -o $@ $^ $(shell pkg-config --libs sdl3 2>/dev/null) -framework OpenGL

build/obj/%.o: gen/%.c runtime/cpu.h
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -c $< -o $@

build/rt/%.o: runtime/%.cpp runtime/rt.h runtime/cpu.h runtime/win32ui.h
	@mkdir -p build/rt
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags sdl3 2>/dev/null) -c $< -o $@

# Metal ray tracing (Objective-C++)
build/rt/rt_metal.o: runtime/rt_metal.mm runtime/rt_metal.h runtime/rt.h
	@mkdir -p build/rt
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

gen: tools/recomp.py tools/fpu.py
	rm -rf gen build/obj
	$(PYTHON) tools/recomp.py "$(EXE)" ghidra/funcs_hi.jsonl gen

# macOS app bundle (build/FerrariVR.app). GAME=<folder> adds the game's icon; EMBED=1 also copies
# the game files into the bundle.
GAME ?= ../extracted
app: build/fvr
	tools/make_app.sh "$(GAME)" $(if $(EMBED),--embed,)

# install the bundle into /Applications
install: app
	rm -rf /Applications/FerrariVR.app
	ditto build/FerrariVR.app /Applications/FerrariVR.app

clean:
	rm -rf build
.PHONY: gen clean app install
