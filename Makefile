# hookshot-launchinst

VERSION_FILE   := resource/version.txt
GIT_SHORT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")

# Retrieve version string from version file
REV := $(shell cat "$(VERSION_FILE)")

# Parse numeric components for windres (MAJOR,MINOR,BUILD,RELEASE)
CLEAN_REV         := $(subst v,,$(firstword $(subst -, ,$(REV))))
WIN_VERSION_MAJOR := $(word 1,$(subst ., ,$(CLEAN_REV)))
WIN_VERSION_MINOR := $(word 2,$(subst ., ,$(CLEAN_REV)))

ifeq ($(GIT_SHORT_HASH),unknown)
WIN_VERSION_BUILD := 0
else
WIN_VERSION_BUILD := $(shell echo $$((16#$(GIT_SHORT_HASH) % 65535)))
endif

WIN_VER := $(WIN_VERSION_MAJOR),$(WIN_VERSION_MINOR),$(WIN_VERSION_BUILD),0

CC32     := /mingw32/bin/i686-w64-mingw32-gcc
CC64     := gcc
WINDRES  := windres
UPX      := upx
UPXFLAGS := --best --quiet

CFLAGS   := -Os -s -std=c11 -Wall -Wextra -municode -lshell32 -lole32

COMMON_SRC  := hks-common.c
COMMON_HDR  := hks-common.h
LAUNCH_SRC  := hks-launch.c
INSTALL_SRC := hookshot-launchinst.c
RC_IN       := resource/hookshot-launchinst.rc.in

RES_OBJ64   := hookshot-launchinst_res64.o
RES_OBJ32   := hookshot-launchinst_res32.o

# Scoped execution helper for 32-bit toolchain steps
RUN32       := PATH=/mingw32/bin:$$PATH

.PHONY: all clean release_dir

all: release_dir hks-launch32.exe hks-launch64.exe release/hookshot-launchinst32.exe release/hookshot-launchinst64.exe

release_dir:
	mkdir -p release

hks-launch32.exe: $(LAUNCH_SRC) $(COMMON_SRC) $(COMMON_HDR)
	$(RUN32) $(CC32) $(CFLAGS) -mwindows -DHKS_LAUNCH_BITS=32 -o $@ $(LAUNCH_SRC) $(COMMON_SRC)

hks-launch64.exe: $(LAUNCH_SRC) $(COMMON_SRC) $(COMMON_HDR)
	$(CC64) $(CFLAGS) -mwindows -DHKS_LAUNCH_BITS=64 -o $@ $(LAUNCH_SRC) $(COMMON_SRC)

$(RES_OBJ64): $(RC_IN) hks-launch32.exe hks-launch64.exe
	sed -e 's/__WIN_VER__/$(WIN_VER)/g' -e 's/__REV__/$(REV)/g' -e 's/__ORIGINAL_FILENAME__/hookshot-launchinst64.exe/g' $(RC_IN) | $(WINDRES) --target=pe-x86-64 -o $@ --output-format=coff

$(RES_OBJ32): $(RC_IN) hks-launch32.exe
	sed -e 's/__WIN_VER__/$(WIN_VER)/g' -e 's/__REV__/$(REV)/g' -e 's/__ORIGINAL_FILENAME__/hookshot-launchinst32.exe/g' $(RC_IN) | $(RUN32) $(WINDRES) --target=pe-i386 -DHKS_INSTALLER_32ONLY -o $@ --output-format=coff

release/hookshot-launchinst64.exe: $(INSTALL_SRC) $(COMMON_SRC) $(COMMON_HDR) $(RES_OBJ64) | release_dir
	$(CC64) $(CFLAGS) -mconsole -o $@ $(INSTALL_SRC) $(COMMON_SRC) $(RES_OBJ64)

release/hookshot-launchinst32.exe: $(INSTALL_SRC) $(COMMON_SRC) $(COMMON_HDR) $(RES_OBJ32) | release_dir
	$(RUN32) $(CC32) $(CFLAGS) -mconsole -DHKS_INSTALLER_32ONLY -o $@ $(INSTALL_SRC) $(COMMON_SRC) $(RES_OBJ32)

clean:
	rm -f *.o hks-launch32.exe hks-launch64.exe release/hookshot-launchinst32.exe release/hookshot-launchinst64.exe