// Static recompilation runtime (see recomp.h and rc_prelude.h).
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/video.h>

#include "rc_prelude.h"

#include <SDL3/SDL_timer.h>

#ifdef SMBJR_RECOMP
extern const struct RecompChunk recomp_chunks[];
extern const int recomp_chunk_count;
extern const int recomp_insn_count;
extern const uint64_t recomp_rom_hash;
#else
static const struct RecompChunk* const recomp_chunks = NULL;
static const int recomp_chunk_count = 0;
static const int recomp_insn_count = 0;
static const uint64_t recomp_rom_hash = 0;
#endif

// ------------------------------------------------------------------ address -> chunk maps
// One table per memory region (top address byte) and CPU mode, one entry per halfword: chunk index + 1.
// ARM and Thumb chunks may cover the same bytes (code and the data around it decode both ways).
enum { REGION_COUNT = 16 };
static uint16_t* s_map[2][REGION_COUNT];
static uint32_t s_mapSize[2][REGION_COUNT];  // bytes covered

static struct GBA* s_gba;
static struct mCore* s_core;
static bool s_attached, s_enabled;
static struct RecompStats s_stats;
static int* s_guarded;  // indexes of the chunks that run from RAM (or the BIOS)
static int s_guardedCount;

static uint32_t region_size(unsigned region, const struct GBA* gba) {
	switch (region) {
	case 0x0: return 0x4000;              // BIOS
	case 0x2: return 0x40000;             // EWRAM
	case 0x3: return 0x8000;              // IWRAM
	case 0x8: return (uint32_t) gba->memory.romSize;  // ROM (wait state 0)
	default: return 0;
	}
}

static const uint8_t* region_live(unsigned region, const struct GBA* gba) {
	switch (region) {
	case 0x0: return (const uint8_t*) gba->memory.bios;
	case 0x2: return (const uint8_t*) gba->memory.wram;
	case 0x3: return (const uint8_t*) gba->memory.iwram;
	case 0x8: return (const uint8_t*) gba->memory.rom;
	default: return NULL;
	}
}

static void maps_free(void) {
	for (int m = 0; m < 2; ++m) {
		for (int r = 0; r < REGION_COUNT; ++r) {
			free(s_map[m][r]);
			s_map[m][r] = NULL;
			s_mapSize[m][r] = 0;
		}
	}
}

static bool maps_build(const struct GBA* gba) {
	maps_free();
	free(s_guarded);
	s_guarded = calloc(recomp_chunk_count + 1, sizeof(int));
	s_guardedCount = 0;
	for (int i = 0; i < recomp_chunk_count; ++i) {
		if (recomp_chunks[i].guard) {
			s_guarded[s_guardedCount++] = i;
		}
	}
	for (int i = 0; i < recomp_chunk_count; ++i) {
		const struct RecompChunk* c = &recomp_chunks[i];
		unsigned r = c->start >> 24;
		uint32_t size = region_size(r, gba);
		if (!size || (c->end & 0xFFFFFF) > size) {
			continue;
		}
		int m = c->thumb;
		if (!s_map[m][r]) {
			s_map[m][r] = calloc(size / 2, sizeof(uint16_t));
			if (!s_map[m][r]) {
				return false;
			}
			s_mapSize[m][r] = size;
		}
		for (uint32_t a = c->start; a < c->end; a += 2) {
			s_map[m][r][(a & 0xFFFFFF) >> 1] = (uint16_t) (i + 1);
		}
	}
	return true;
}

static inline const struct RecompChunk* lookup(uint32_t pc, int thumb) {
	unsigned r = pc >> 24;
	uint32_t off = pc & 0xFFFFFF;
	if (r >= REGION_COUNT || off >= s_mapSize[thumb][r]) {
		return NULL;
	}
	uint16_t id = s_map[thumb][r][off >> 1];
	return id ? &recomp_chunks[id - 1] : NULL;
}

static inline bool guard_ok(const struct RecompChunk* c) {
	const uint8_t* live = region_live(c->start >> 24, s_gba);
	return live && !memcmp(live + (c->start & 0xFFFFFF), c->guard, c->guardLen);
}

// Several overlays can be loaded at the same RAM address over time; every version the recompiler saw
// is a chunk, and the one whose bytes match the live memory runs (see guarded_alternative).

static const struct RecompChunk* guarded_alternative(uint32_t pc, int thumb) {
	for (int i = 0; i < s_guardedCount; ++i) {
		const struct RecompChunk* c = &recomp_chunks[s_guarded[i]];
		if (c->thumb == thumb && c->start <= pc && pc < c->end && guard_ok(c)) {
			return c;
		}
	}
	return NULL;
}

// RAM snapshots taken when unknown RAM code runs, so learn.py can find its ROM source.
enum { SNAP_MAX = 8 };
static uint8_t* s_snap[SNAP_MAX];
static int s_snapCount;

static void snapshot_ram_code(uint32_t pc) {
	if ((pc >> 24) != 0x3 && (pc >> 24) != 0x2) {
		return;
	}
	const uint8_t* live = region_live(pc >> 24, s_gba);
	uint32_t off = pc & 0xFFFFFF;
	for (int i = 0; i < s_snapCount; ++i) {
		if (s_snap[i][0] == (pc >> 24) && !memcmp(s_snap[i] + 1 + off, live + off, 16)) {
			return;  // this code is already in a snapshot
		}
	}
	if (s_snapCount == SNAP_MAX) {
		return;
	}
	uint32_t size = region_size(pc >> 24, s_gba);
	uint8_t* snap = malloc(1 + size);
	if (snap) {
		snap[0] = (uint8_t) (pc >> 24);
		memcpy(snap + 1, live, size);
		s_snap[s_snapCount++] = snap;
	}
}

// ------------------------------------------------------------------ miss log (open addressing)
enum { MISS_CAP = 1 << 15 };
static uint32_t s_missKey[MISS_CAP];  // pc | thumb (pc is always even)
static uint32_t s_missCount[MISS_CAP];

static bool miss(uint32_t pc, int thumb) {  // true the first time this address misses
	++s_stats.interpreted;
	uint32_t key = pc | (uint32_t) thumb;
	uint32_t h = (key * 2654435761u) >> 17;
	for (int probe = 0; probe < 64; ++probe, h = (h + 1) & (MISS_CAP - 1)) {
		if (s_missKey[h] == key) {
			++s_missCount[h];
			return false;
		}
		if (!s_missKey[h]) {
			s_missKey[h] = key;
			s_missCount[h] = 1;
			++s_stats.missAddresses;
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------------ run loop
static void run_loop(struct ARMCore* cpu) {
	while (cpu->cycles < cpu->nextEvent) {
		int thumb = cpu->executionMode == MODE_THUMB;
		uint32_t pc = (uint32_t) cpu->gprs[ARM_PC] - (thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM);
		const struct RecompChunk* c = lookup(pc, thumb);
		if (c) {
			if (c->guard && !guard_ok(c)) {
				c = guarded_alternative(pc, thumb);
				if (!c) {
					++s_stats.guardFails;
				}
			}
			if (c && c->fn(cpu, pc)) {
				++s_stats.nativeRuns;
				continue;
			}
		}
		if (miss(pc, thumb)) {
			snapshot_ram_code(pc);
		}
		ARMRun(cpu);  // one interpreted instruction (events are not due here)
	}
	cpu->irqh.processEvents(cpu);
}

// Same loop as mGBA's _GBACoreRunFrame, with the CPU replaced by the recompiled code.
static void run_frame(struct mCore* core) {
	struct GBA* gba = core->board;
	if (!s_enabled) {
		uint32_t frameCounter = gba->video.frameCounter;
		uint32_t startCycle = mTimingCurrentTime(&gba->timing);
		while (gba->video.frameCounter == frameCounter &&
		       mTimingCurrentTime(&gba->timing) - startCycle < VIDEO_TOTAL_LENGTH + VIDEO_HORIZONTAL_LENGTH) {
			ARMRunLoop(core->cpu);
		}
		return;
	}
	uint32_t frameCounter = gba->video.frameCounter;
	uint32_t startCycle = mTimingCurrentTime(&gba->timing);
	while (gba->video.frameCounter == frameCounter &&
	       mTimingCurrentTime(&gba->timing) - startCycle < VIDEO_TOTAL_LENGTH + VIDEO_HORIZONTAL_LENGTH) {
		run_loop(core->cpu);
	}
}

static void run_loop_core(struct mCore* core) {
	if (s_enabled) {
		run_loop(core->cpu);
	} else {
		ARMRunLoop(core->cpu);
	}
}

// ------------------------------------------------------------------ profiling
// Time spent in the GBA hardware (video, audio, DMA, timers: everything mGBA does between CPU slices),
// measured the same way in both modes so the CPU share can be compared.
static void (*s_processEvents)(struct ARMCore*);
static uint64_t s_hwNs;

static void timed_process_events(struct ARMCore* cpu) {
	uint64_t t0 = SDL_GetTicksNS();
	s_processEvents(cpu);
	s_hwNs += SDL_GetTicksNS() - t0;
}

uint64_t recomp_take_hw_ns(void) {
	uint64_t v = s_hwNs;
	s_hwNs = 0;
	return v;
}

// ------------------------------------------------------------------ public API
bool recomp_available(void) {
	return recomp_chunk_count > 0;
}

static uint64_t rom_hash(const uint8_t* data, size_t size) {
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i + 4 <= size; i += 4) {
		uint32_t w = (uint32_t) data[i] | (uint32_t) data[i + 1] << 8 | (uint32_t) data[i + 2] << 16 |
		             (uint32_t) data[i + 3] << 24;
		h ^= w;
		h *= 0x100000001b3ULL;
	}
	return h;
}

bool recomp_attach(struct mCore* core) {
	s_attached = s_enabled = false;
	if (!recomp_available()) {
		return false;
	}
	struct GBA* gba = core->board;
	if (rom_hash((const uint8_t*) gba->memory.rom, gba->memory.romSize) != recomp_rom_hash) {
		fprintf(stderr, "recomp: ROM differs from the one this build was recompiled from; using the interpreter\n");
		return false;
	}
	if (!maps_build(gba)) {
		return false;
	}
	s_gba = gba;
	s_core = core;
	core->runFrame = run_frame;
	struct ARMCore* cpu = core->cpu;
	if (cpu->irqh.processEvents != timed_process_events) {
		s_processEvents = cpu->irqh.processEvents;
		cpu->irqh.processEvents = timed_process_events;
	}
	core->runLoop = run_loop_core;
	memset(&s_stats, 0, sizeof(s_stats));
	memset(s_missKey, 0, sizeof(s_missKey));
	s_attached = s_enabled = true;
	return true;
}

void recomp_set_enabled(bool on) {
	s_enabled = on && s_attached;
}

bool recomp_enabled(void) {
	return s_enabled;
}

void recomp_stats(struct RecompStats* out) {
	*out = s_stats;
	out->insnCount = recomp_insn_count;
	out->chunkCount = recomp_chunk_count;
}

// RAM code is copied from the ROM at boot; find where so the recompiler can read it from the ROM.
static uint32_t find_rom_source(uint32_t pc) {
	unsigned r = pc >> 24;
	const uint8_t* live = region_live(r, s_gba);
	uint32_t size = region_size(r, s_gba);
	uint32_t off = pc & 0xFFFFFF;
	if (!live || r == 0x8 || off + 16 > size) {
		return 0;
	}
	const uint8_t* rom = (const uint8_t*) s_gba->memory.rom;
	size_t romSize = s_gba->memory.romSize;
	// Match 32 bytes around the address (fewer at the end of the region).
	uint32_t len = size - off < 32 ? size - off : 32;
	for (size_t i = 0; i + len <= romSize; i += 2) {
		if (rom[i] == live[off] && !memcmp(rom + i, live + off, len)) {
			return 0x08000000u + (uint32_t) i;
		}
	}
	return 0;
}

bool recomp_write_misses(const char* path) {
	if (!s_gba) {
		return false;
	}
	FILE* f = fopen(path, "w");
	if (!f) {
		return false;
	}
	fprintf(f, "# Addresses that ran on the interpreter (mode address [rom_source]  # count)\n");
	for (int i = 0; i < MISS_CAP; ++i) {
		if (!s_missKey[i]) {
			continue;
		}
		int thumb = s_missKey[i] & 1;
		uint32_t pc = s_missKey[i] & ~1u;
		uint32_t src = find_rom_source(pc);
		if (src) {
			fprintf(f, "%c %08X %08X  # %u\n", thumb ? 'T' : 'A', pc, src, s_missCount[i]);
		} else {
			fprintf(f, "%c %08X  # %u\n", thumb ? 'T' : 'A', pc, s_missCount[i]);
		}
	}
	fclose(f);
	// RAM snapshots (they hold game code and data: keep them local, never commit them).
	char dump[1024];
	for (int i = 0; i < s_snapCount; ++i) {
		snprintf(dump, sizeof(dump), "%s.%s.%d", path, s_snap[i][0] == 3 ? "iwram" : "ewram", i);
		if ((f = fopen(dump, "wb"))) {
			fwrite(s_snap[i] + 1, 1, region_size(s_snap[i][0], s_gba), f);
			fclose(f);
		}
	}
	return true;
}
