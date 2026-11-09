ROOT := $(CURDIR)
TOOLS := $(abspath ../build-tools)
PS3DEV ?= $(TOOLS)/ps3dev
PSL1GHT ?= $(PS3DEV)
export PS3DEV PSL1GHT
export PATH := $(PS3DEV)/bin:$(PS3DEV)/ppu/bin:$(PS3DEV)/spu/bin:$(PATH)
MBED := $(TOOLS)/mbedtls-2.28.10
PORT := $(PS3DEV)/portlibs/ppu
CORE := src/platform.c src/store.c src/network.c src/nat.c src/session.c src/relay.c src/bore.c src/voice_core.c vendor/cJSON.c vendor/monocypher.c
INCS := -Iinclude -Ivendor
CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-misleading-indentation -DCJSON_NESTING_LIMIT=16 $(INCS)
HOSTLIBS := $(MBED)/library/libmbedtls.a $(MBED)/library/libmbedx509.a $(MBED)/library/libmbedcrypto.a -lpthread -lm
PPUCC := $(PS3DEV)/ppu/bin/ppu-gcc
PPUFLAGS := $(CFLAGS) -D__PPU__ -mcpu=cell -mhard-float -ffunction-sections -fdata-sections -I$(PS3DEV)/ppu/include -I$(PORT)/include -I$(PORT)/include/freetype2
PPULIBS := -L$(PORT)/lib -L$(PS3DEV)/ppu/lib -lfreetype -lpng -lz -lmbedtls -lmbedx509 -lmbedcrypto -laudio -lrsx -lgcm_sys -lsysutil -lio -lnet -lnetctl -lsysmodule -lpthread -lrt -llv2 -lm
.PHONY: host test ps3 clean
host: build/orbit-test-host
build:
	mkdir -p build
build/orbit-test-host: $(CORE) tools/host.c | build
	$(CC) $(CFLAGS) -I$(MBED)/include $^ $(HOSTLIBS) -o $@ -lm
build/test-core: $(CORE) tests/test_core.c | build
	$(CC) $(CFLAGS) -I$(MBED)/include $^ $(HOSTLIBS) -o $@ -lm
build/test-network: $(CORE) tests/test_network.c | build
	$(CC) $(CFLAGS) -I$(MBED)/include $^ $(HOSTLIBS) -o $@ -lm
build/test-app: $(CORE) src/app.c src/voice_host.c tests/test_app.c | build
	$(CC) $(CFLAGS) -I$(MBED)/include $^ $(HOSTLIBS) -o $@ -lm
build/test-sessions: $(CORE) tests/test_sessions.c | build
	$(CC) $(CFLAGS) -I$(MBED)/include $^ $(HOSTLIBS) -o $@ -lm
test: build/test-core build/test-network build/test-app build/test-sessions
	./build/test-core
	./build/test-network
	./build/test-sessions
	./build/test-app
preview: build/preview
	./build/preview
build/preview: src/ui.c tools/preview.c | build
	$(CC) $(CFLAGS) -I$(PORT)/include -I$(PORT)/include/freetype2 $^ /usr/lib/x86_64-linux-gnu/libfreetype.so.6 -lm -o $@
ps3: build/orbit.elf
PPUSRC := $(CORE) src/app.c src/ui.c src/main_ps3.c src/rsxutil.c src/voice_ps3.c
PPUOBJ := $(patsubst %.c,build/ppu/%.o,$(PPUSRC))
build/ppu/%.o: %.c
	mkdir -p $(dir $@)
	$(PPUCC) $(PPUFLAGS) -MMD -MP -c $< -o $@
build/orbit.elf: $(PPUOBJ) build/mic-import.o | build
	$(PPUCC) $^ -Wl,--gc-sections $(PPULIBS) -o $@
-include $(PPUOBJ:.o=.d)
clean:
	rm -f build/orbit-test-host build/test-core build/orbit.elf

# Isolated audio diagnostic: no additional imports in the chat executable.
probe: build/audio-probe.elf
build/mic-import.o: vendor/mic-import/exports.S vendor/mic-import/libexport.c vendor/mic-import/exports.h vendor/mic-import/config.h | build
	$(PPUCC) -S -m32 -Ivendor/mic-import vendor/mic-import/libexport.c -o build/mic-libexport.S
	$(PPUCC) -c build/mic-libexport.S -o build/mic-libexport.o
	$(PPUCC) -c -mregnames -mcpu=cell -D__ASSEMBLY__ -Ivendor/mic-import -I$(PS3DEV)/ppu/include vendor/mic-import/exports.S -o build/mic-exports.o
	$(PS3DEV)/ppu/bin/ppu-ld -r build/mic-exports.o build/mic-libexport.o -o $@
build/audio-probe.elf: build/ppu/src/audio_probe.o build/ppu/src/ui.o build/ppu/src/rsxutil.o build/mic-import.o
	$(PPUCC) $^ -Wl,--gc-sections $(PPULIBS) -o $@
