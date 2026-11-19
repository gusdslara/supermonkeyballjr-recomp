// Static recompilation runtime: runs the game's recompiled native code on top of mGBA's hardware
// (video, audio, DMA, timers, interrupts, saves). Addresses without native code fall back to the
// interpreter one instruction at a time and are counted, so the coverage can be learned and fixed.
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct mCore;

// True when this build contains recompiled code.
bool recomp_available(void);

// Hooks the core's frame loop. Returns false (and leaves the interpreter in charge) when the build
// has no recompiled code or the loaded ROM is not the one it was generated from.
bool recomp_attach(struct mCore* core);

// Forget which RAM code was verified (call after reset or loading a savestate).
void recomp_invalidate(void);

// Native (true) or interpreter (false) execution; attach must have succeeded for native.
void recomp_set_enabled(bool on);
bool recomp_enabled(void);

struct RecompStats {
	uint64_t nativeRuns;     // chunk entries
	uint64_t interpreted;    // instructions run by the interpreter fallback
	uint32_t missAddresses;  // distinct addresses that fell back
	uint32_t guardFails;     // RAM chunks whose live bytes no longer matched
	int insnCount;           // recompiled instructions in this build
	int chunkCount;
};
void recomp_stats(struct RecompStats* out);

// Nanoseconds spent in the GBA hardware emulation since the last call (profiling).
uint64_t recomp_take_hw_ns(void);

// Writes every address that fell back to the interpreter, in tools/recomp/entries.txt format
// (RAM code is written as "mode ram_addr rom_source_addr" when its bytes are found in the ROM).
bool recomp_write_misses(const char* path);
