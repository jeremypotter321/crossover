# Cross-compiles the probe harness for 32-bit Windows from macOS/Linux.
#
#   brew install mingw-w64
#   make
#   make run          # inject into the Fable install in the Whisky bottle
#   make log          # show the probe output
#
# 32-bit is mandatory: Fable.exe is i386, and a 64-bit DLL cannot be injected
# into it.

CC      := i686-w64-mingw32-gcc
CFLAGS  := -m32 -O2 -Wall -Wextra

BOTTLE  := $(HOME)/Library/Containers/com.isaacmarovitz.Whisky/Bottles/3230151B-BEA6-48C5-8C7D-A7580586BF68
GAME    := $(BOTTLE)/drive_c/Games/Fable
WHISKY  := /Applications/Whisky.app/Contents/Resources/WhiskyCmd

SRC ?= probe.c

all: probe.dll inject.exe attach.exe

probe.dll: $(SRC)
	$(CC) $(CFLAGS) -shared -o $@ $< -Wl,--enable-stdcall-fixup

inject.exe: inject.c
	$(CC) $(CFLAGS) -o $@ $<

attach.exe: attach.c
	$(CC) $(CFLAGS) -o $@ $<

# Copy both into the game directory and run the injector there. The probe writes
# probe.log relative to the process working directory, which is the game dir.
install: all
	cp probe.dll inject.exe attach.exe "$(GAME)/"

run: install
	cd "$(GAME)" && eval "$$($(WHISKY) shellenv Fable)" && wine64 inject.exe Fable.exe probe.dll

# Attach to the game that is already running, so an iteration does not cost a
# relaunch (and, with no save games, a replay of the intro). LoadLibrary
# ignores a DLL that is already mapped, so each attach gets a fresh filename.
ATTACH_N ?= $(shell date +%H%M%S)
attach: all
	cp probe.dll "$(GAME)/probe-$(ATTACH_N).dll"
	cp attach.exe "$(GAME)/"
	cd "$(GAME)" && eval "$$($(WHISKY) shellenv Fable)" && wine64 attach.exe probe-$(ATTACH_N).dll

log:
	@cat "$(GAME)/probe.log" 2>/dev/null || echo "no probe.log yet"

clean:
	rm -f probe.dll inject.exe
	rm -f "$(GAME)/probe.dll" "$(GAME)/inject.exe" "$(GAME)/probe.log"

.PHONY: all install run log clean
