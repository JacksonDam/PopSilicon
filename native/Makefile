BUILD_DIR := build
CLANG := $(shell xcrun --find clang)
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
CFLAGS := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=11.0 -O2 -g -Wall -Wextra -Werror
LDFLAGS := -Wl,-no_pie,-pagezero_size,0x1000,-segaddr,__TEXT,0x100000000,-segaddr,__LEGACY,0x1000,-segprot,__LEGACY,rwx,rwx

.PHONY: all clean run

all: $(BUILD_DIR)/game_loader

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/game_loader: $(wildcard src/*.h src/*.inc) src/game_loader.c src/game_profile.c src/macho_loader.c src/compat_runtime.c src/audio_bridge.c src/objc_bridge.m src/arb_program_guard.c src/arb_sampler_usage.c src/mode32_switch.S src/import_gateway.S src/address_space_reserve.S | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $(LDFLAGS) $(filter %.c %.m %.S,$^) -Lvendor/bass -lbass -Wl,-rpath,@executable_path -framework Cocoa -framework Carbon -framework OpenGL -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework IOKit -o $@
	codesign --force --sign - $@

run: $(BUILD_DIR)/game_loader
	arch -x86_64 ./$(BUILD_DIR)/game_loader

clean:
	rm -f $(BUILD_DIR)/game_loader
