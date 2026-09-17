# hookshot-launchinst
#
# Produces four binaries:
#   hks-launch32.exe                  32-bit launcher, deployed as <game>.exe (GUI)
#   hks-launch64.exe                  64-bit launcher, deployed as <game>.exe (GUI)
#   release/hookshot-launchinst32.exe Installer / uninstaller (32-bit Console)
#   release/hookshot-launchinst64.exe Installer / uninstaller (64-bit Console)

REV      := $(shell sh -c 'date +"%Y,%m,%d"')

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
	sed -e 's/__REV__/$(REV)/g' -e 's/__ORIGINAL_FILENAME__/hookshot-launchinst64.exe/g' $(RC_IN) | $(WINDRES) --target=pe-x86-64 -o $@ --output-format=coff

$(RES_OBJ32): $(RC_IN) hks-launch32.exe
	sed -e 's/__REV__/$(REV)/g' -e 's/__ORIGINAL_FILENAME__/hookshot-launchinst32.exe/g' $(RC_IN) | $(RUN32) $(WINDRES) --target=pe-i386 -DHKS_INSTALLER_32ONLY -o $@ --output-format=coff

release/hookshot-launchinst64.exe: $(INSTALL_SRC) $(COMMON_SRC) $(COMMON_HDR) $(RES_OBJ64) | release_dir
	$(CC64) $(CFLAGS) -mconsole -o $@ $(INSTALL_SRC) $(COMMON_SRC) $(RES_OBJ64)

release/hookshot-launchinst32.exe: $(INSTALL_SRC) $(COMMON_SRC) $(COMMON_HDR) $(RES_OBJ32) | release_dir
	$(RUN32) $(CC32) $(CFLAGS) -mconsole -DHKS_INSTALLER_32ONLY -o $@ $(INSTALL_SRC) $(COMMON_SRC) $(RES_OBJ32)

clean:
	rm -f *.o hks-launch32.exe hks-launch64.exe release/hookshot-launchinst32.exe release/hookshot-launchinst64.exe