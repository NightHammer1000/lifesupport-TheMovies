# Makefile for movies_fix.asi.
#
# Prerequisites:
#   - MSYS2 mingw32 toolchain at  C:/msys64/mingw32
#   - Static FFmpeg at            C:/tmp/ffbuild/install
#   - libmpv i686 dev archive in  lib/libmpv/  (libmpv-2.dll, libmpv.dll.a, include/mpv/*.h)
#
# Targets:
#   make            build/movies_fix.asi
#   make deploy     copy ASI + libmpv-2.dll into ../  (the game directory)
#   make clean
#   make info       print the resolved DLL imports of the built ASI

CC      := C:/msys64/mingw32/bin/gcc.exe
LD      := C:/msys64/mingw32/bin/i686-w64-mingw32-gcc.exe
OBJDUMP := C:/msys64/mingw32/bin/objdump.exe

# cc1.exe (spawned by gcc) needs the toolchain's runtime DLLs (libgcc_s,
# libisl, libmpc, etc.) on PATH. Without this, recent MSYS2 builds fail
# with "libgcc_s_dw2-1.dll not found" when invoked from a shell whose
# PATH doesn't include mingw32/bin.
export PATH := C:/msys64/mingw32/bin:$(PATH)

BUILDDIR := build
SRCDIR   := src
MHDIR    := lib/minhook
MPVDIR   := lib/libmpv
FFDIR    := C:/tmp/ffbuild/install

TARGET   := $(BUILDDIR)/movies_fix.asi

GAME_DIR := ..

# --- sources ---
SRC := $(SRCDIR)/main.c          \
       $(SRCDIR)/log.c           \
       $(SRCDIR)/sync_reader.c   \
       $(SRCDIR)/profile_mgr.c   \
       $(SRCDIR)/media_props.c   \
       $(SRCDIR)/nss_buffer.c    \
       $(SRCDIR)/ds_filter.c     \
       $(SRCDIR)/ds_output_pin.c \
       $(SRCDIR)/ds_fakegraph.c  \
       $(SRCDIR)/asf_writer.c    \
       $(SRCDIR)/mpv_player.c

MH_SRC := $(MHDIR)/src/hook.c       \
          $(MHDIR)/src/buffer.c     \
          $(MHDIR)/src/trampoline.c \
          $(MHDIR)/src/hde/hde32.c

OBJ    := $(patsubst %.c,$(BUILDDIR)/%.o,$(notdir $(SRC) $(MH_SRC)))

# --- flags ---
# -MMD -MP makes gcc emit a .d sidecar next to each .o that lists every
# header the .c included. The `-include` at the bottom of this file
# pulls those .d files into make's dependency graph, so editing a
# header (e.g. ds_types.h) correctly forces every dependent .c to
# rebuild on the next `make`. Without this, header-only edits look
# like a no-op build.
CFLAGS := -m32 -O2 -Wall \
          -Wno-unknown-pragmas -Wno-misleading-indentation \
          -I$(MHDIR) -I$(MHDIR)/include -I$(SRCDIR) \
          -I$(FFDIR)/include -I$(MPVDIR)/include \
          -DWIN32_LEAN_AND_MEAN \
          -MMD -MP

# -static-libstdc++ folds the C++ runtime into the ASI so we don't ship
# libstdc++-6.dll alongside. Same for libgcc. openh264.a is C++, hence
# the libstdc++ dependency.
LDFLAGS := -m32 -shared -static-libgcc -static-libstdc++

LDLIBS  := -L$(FFDIR)/lib -L/c/msys64/mingw32/lib -Wl,-Bstatic \
               -lavformat -lavcodec -lswscale -lswresample -lavutil \
               -lopenh264 -lstdc++ -lsupc++ \
               -lz -lwinpthread \
           -Wl,-Bdynamic \
               -L$(MPVDIR) -lmpv \
               -lole32 -loleaut32 -luuid -lws2_32 -lbcrypt -lm -lsecur32 -lwinmm

# --- rules ---
.PHONY: all clean info deploy
.DEFAULT_GOAL := all

all: $(TARGET)
	@echo "=== Built $(TARGET) ==="

# generic compile rule for $(SRCDIR)/
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# minhook compile rules (sources live under $(MHDIR)/src and $(MHDIR)/src/hde)
$(BUILDDIR)/hook.o:       $(MHDIR)/src/hook.c       | $(BUILDDIR) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILDDIR)/buffer.o:     $(MHDIR)/src/buffer.c     | $(BUILDDIR) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILDDIR)/trampoline.o: $(MHDIR)/src/trampoline.c | $(BUILDDIR) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILDDIR)/hde32.o:      $(MHDIR)/src/hde/hde32.c  | $(BUILDDIR) ; $(CC) $(CFLAGS) -c -o $@ $<

# link
$(TARGET): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

deploy: $(TARGET)
	cp $(TARGET) $(GAME_DIR)/movies_fix.asi
	cp $(MPVDIR)/libmpv-2.dll $(GAME_DIR)/libmpv-2.dll
	@echo "Deployed to $(GAME_DIR)"

clean:
	rm -f $(BUILDDIR)/*.o $(BUILDDIR)/*.d $(TARGET)

info: $(TARGET)
	@$(OBJDUMP) -p $(TARGET) | grep "DLL Name" | sort -u

# Pull in auto-generated header dependencies (one .d per .o, written
# by -MMD). The leading dash prevents make from erroring on the first
# build before any .d exists yet.
-include $(OBJ:.o=.d)
