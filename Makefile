# hookshot-launchinst

VERSION_FILE := resource/version.txt

# Version string written by version.sh, e.g. v1.2.3
REV := $(shell cat "$(VERSION_FILE)" 2>/dev/null)

# Parse numeric components for windres (MAJOR,MINOR,BUILD,RELEASE)
CLEAN_REV         := $(patsubst v%,%,$(REV))
WIN_VERSION_MAJOR := $(word 1,$(subst ., ,$(CLEAN_REV)))
WIN_VERSION_MINOR := $(word 2,$(subst ., ,$(CLEAN_REV)))
WIN_VERSION_BUILD := $(word 3,$(subst ., ,$(CLEAN_REV)))

WIN_VER := $(WIN_VERSION_MAJOR),$(WIN_VERSION_MINOR),$(WIN_VERSION_BUILD),0

CC32     := /mingw32/bin/i686-w64-mingw32-gcc
CC64     := gcc
WINDRES  := windres
UPX      := upx
UPXFLAGS := --best --quiet

CFLAGS   := -Os -s -std=c11 -Wall -Wextra -municode
LDLIBS   := -lshell32 -lole32
# The launcher's compatibility prompt is a TaskDialog (comctl32 v6, activated
# by resource/hks-launch.manifest).
LAUNCH_LIBS := $(LDLIBS) -lcomctl32

COMMON_SRC  := hks-common.c
COMMON_HDR  := hks-common.h
LAUNCH_SRC  := hks-launch.c
INSTALL_SRC := hookshot-launchinst.c
RC_IN       := resource/hookshot-launchinst.rc.in
LAUNCH_RC   := resource/hks-launch.rc
MANIFEST    := resource/hks-launch.manifest

RES_OBJ64   := hookshot-launchinst_res64.o
RES_OBJ32   := hookshot-launchinst_res32.o
LAUNCH_RES_OBJ64 := hks-launch_res64.o
LAUNCH_RES_OBJ32 := hks-launch_res32.o

# Scoped execution helper for 32-bit toolchain steps
RUN32       := PATH=/mingw32/bin:$$PATH

.PHONY: all clean release_dir

all: release_dir hks-launch32.exe hks-launch64.exe release/hookshot-launchinst32.exe release/hookshot-launchinst64.exe

release_dir:
	mkdir -p release

hks-launch32.exe: $(LAUNCH_SRC) $(COMMON_SRC) $(COMMON_HDR) $(LAUNCH_RES_OBJ32)
	$(RUN32) $(CC32) $(CFLAGS) -mwindows -DHKS_LAUNCH_BITS=32 -o $@ $(LAUNCH_SRC) $(COMMON_SRC) $(LAUNCH_RES_OBJ32) $(LAUNCH_LIBS)

hks-launch64.exe: $(LAUNCH_SRC) $(COMMON_SRC) $(COMMON_HDR) $(LAUNCH_RES_OBJ64)
	$(CC64) $(CFLAGS) -mwindows -DHKS_LAUNCH_BITS=64 -o $@ $(LAUNCH_SRC) $(COMMON_SRC) $(LAUNCH_RES_OBJ64) $(LAUNCH_LIBS)

$(LAUNCH_RES_OBJ64): $(LAUNCH_RC) $(MANIFEST)
	$(WINDRES) -I resource --target=pe-x86-64 -o $@ --output-format=coff $(LAUNCH_RC)

$(LAUNCH_RES_OBJ32): $(LAUNCH_RC) $(MANIFEST)
	$(RUN32) $(WINDRES) -I resource --target=pe-i386 -o $@ --output-format=coff $(LAUNCH_RC)

$(RES_OBJ64): $(RC_IN) hks-launch32.exe hks-launch64.exe
	sed -e 's/__WIN_VER__/$(WIN_VER)/g' -e 's/__REV__/$(REV)/g' -e 's/__ORIGINAL_FILENAME__/hookshot-launchinst64.exe/g' $(RC_IN) | $(WINDRES) --target=pe-x86-64 -o $@ --output-format=coff

$(RES_OBJ32): $(RC_IN) hks-launch32.exe
	sed -e 's/__WIN_VER__/$(WIN_VER)/g' -e 's/__REV__/$(REV)/g' -e 's/__ORIGINAL_FILENAME__/hookshot-launchinst32.exe/g' $(RC_IN) | $(RUN32) $(WINDRES) --target=pe-i386 -DHKS_INSTALLER_32ONLY -o $@ --output-format=coff

release/hookshot-launchinst64.exe: $(INSTALL_SRC) $(COMMON_SRC) $(COMMON_HDR) $(RES_OBJ64) | release_dir
	$(CC64) $(CFLAGS) -mconsole -o $@ $(INSTALL_SRC) $(COMMON_SRC) $(RES_OBJ64) $(LDLIBS)

release/hookshot-launchinst32.exe: $(INSTALL_SRC) $(COMMON_SRC) $(COMMON_HDR) $(RES_OBJ32) | release_dir
	$(RUN32) $(CC32) $(CFLAGS) -mconsole -DHKS_INSTALLER_32ONLY -o $@ $(INSTALL_SRC) $(COMMON_SRC) $(RES_OBJ32) $(LDLIBS)

clean:
	rm -f *.o hks-launch32.exe hks-launch64.exe release/hookshot-launchinst32.exe release/hookshot-launchinst64.exe