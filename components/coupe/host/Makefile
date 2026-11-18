Z80 := ../../../../esp32emu/components/z80
DEBUGGER := ../../../../esp32emu/components/emudbg
CPPFLAGS += -DEMUDBG_OFF -DCONFIG_Z80_CONTENTION=1 -I../include -I$(Z80)/include -I$(DEBUGGER)/include
CFLAGS ?= -O2 -g -Wall -Wextra

.PHONY: all
all: build/coupe_host

build/coupe_host: host_main.c ../coupe.c ../floppy.c ../sound.c $(Z80)/z80.c ../include/coupe.h ../floppy.h ../sound.h ../coupe_lock.h Makefile
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(filter %.c,$^) $(LDFLAGS) -o $@
