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

all: probe.dll inject.exe

probe.dll: probe.c
	$(CC) $(CFLAGS) -shared -o $@ $< -Wl,--enable-stdcall-fixup

inject.exe: inject.c
	$(CC) $(CFLAGS) -o $@ $<

# Copy both into the game directory and run the injector there. The probe writes
# probe.log relative to the process working directory, which is the game dir.
install: all
	cp probe.dll inject.exe "$(GAME)/"

run: install
	cd "$(GAME)" && eval "$$($(WHISKY) shellenv Fable)" && wine64 inject.exe Fable.exe probe.dll

log:
	@cat "$(GAME)/probe.log" 2>/dev/null || echo "no probe.log yet"

clean:
	rm -f probe.dll inject.exe
	rm -f "$(GAME)/probe.dll" "$(GAME)/inject.exe" "$(GAME)/probe.log"

.PHONY: all install run log clean
