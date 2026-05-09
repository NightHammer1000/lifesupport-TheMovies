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
       $(SRCDIR)/mpv_player.c

MH_SRC := $(MHDIR)/src/hook.c       \
          $(MHDIR)/src/buffer.c     \
          $(MHDIR)/src/trampoline.c \
          $(MHDIR)/src/hde/hde32.c

OBJ    := $(patsubst %.c,$(BUILDDIR)/%.o,$(notdir $(SRC) $(MH_SRC)))

# --- flags ---
CFLAGS := -m32 -O2 -Wall \
          -Wno-unknown-pragmas -Wno-misleading-indentation \
          -I$(MHDIR) -I$(MHDIR)/include -I$(SRCDIR) \
          -I$(FFDIR)/include -I$(MPVDIR)/include \
          -DWIN32_LEAN_AND_MEAN

LDFLAGS := -m32 -shared -static-libgcc

LDLIBS  := -L$(FFDIR)/lib -Wl,-Bstatic \
               -lavformat -lavcodec -lswscale -lswresample -lavutil \
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
	rm -f $(BUILDDIR)/*.o $(TARGET)

info: $(TARGET)
	@$(OBJDUMP) -p $(TARGET) | grep "DLL Name" | sort -u
