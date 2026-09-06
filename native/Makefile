BUILD_DIR := build
CLANG := $(shell xcrun --find clang)
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
CFLAGS := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=11.0 -O2 -g -Wall -Wextra -Werror
# __LINKEDIT must sit above the Mach header.  Left to itself the linker places
# it directly after the __LEGACY reservation, at 0x80000000, below __TEXT.
# dyld keeps the header-relative position of the export trie in 32 bits, so
# on macOS 26 a negative offset wraps and the first dlsym() that searches the
# main executable (AppKit does one during window setup) reads 0x180000000+
# and the game dies with SIGSEGV right after entering main.
LDFLAGS := -Wl,-no_pie,-pagezero_size,0x1000,-segaddr,__TEXT,0x100000000,-segaddr,__LEGACY,0x1000,-segprot,__LEGACY,rwx,rwx,-segaddr,__LINKEDIT,0x110000000

.PHONY: all clean run

all: $(BUILD_DIR)/game_loader

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/game_loader: Makefile $(wildcard src/*.h src/*.inc) src/game_loader.c src/game_profile.c src/macho_loader.c src/compat_runtime.c src/audio_bridge.c src/objc_bridge.m src/arb_program_guard.c src/arb_sampler_usage.c src/mode32_switch.S src/import_gateway.S src/address_space_reserve.S | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $(LDFLAGS) $(filter %.c %.m %.S,$^) -Lvendor/bass -lbass -Wl,-rpath,@executable_path -framework Cocoa -framework Carbon -framework OpenGL -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework IOKit -o $@
	@text=$$(otool -l $@ | awk '/segname __TEXT$$/ { getline; print $$2; exit }'); \
	linkedit=$$(otool -l $@ | awk '/segname __LINKEDIT$$/ { getline; print $$2; exit }'); \
	if [ -z "$$text" ] || [ -z "$$linkedit" ] || [ $$((linkedit)) -le $$((text)) ]; then \
		echo "game_loader: __LINKEDIT ($$linkedit) must be above __TEXT ($$text); this linker ignored -segaddr __LINKEDIT" >&2; \
		rm -f $@; exit 1; \
	fi
	codesign --force --sign - $@

run: $(BUILD_DIR)/game_loader
	arch -x86_64 ./$(BUILD_DIR)/game_loader

clean:
	rm -f $(BUILD_DIR)/game_loader
