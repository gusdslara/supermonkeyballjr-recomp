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

// Code guards: a guarded chunk is checked against live memory once, and again only after something
// wrote onto RAM code bytes (rc_codeGen changes). See rc_code_write in rc_prelude.h.
uint8_t* rc_codeBits[16];
uint32_t rc_codeGen = 1;
static uint32_t* s_validGen;
static bool s_noGuardCache;  // RC_NOCACHE=1: check every entry (debugging)  // per chunk: rc_codeGen value at its last successful check

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
	for (int r = 0; r < 16; ++r) {
		free(rc_codeBits[r]);
		rc_codeBits[r] = NULL;
	}
	free(s_validGen);
	s_validGen = calloc(recomp_chunk_count + 1, sizeof(uint32_t));
	if (!s_validGen) {
		return false;
	}
	++rc_codeGen;
	free(s_guarded);
	s_guarded = calloc(recomp_chunk_count + 1, sizeof(int));
	s_guardedCount = 0;
	for (int i = 0; i < recomp_chunk_count; ++i) {
		const struct RecompChunk* c = &recomp_chunks[i];
		if (!c->guard) {
			continue;
		}
		s_guarded[s_guardedCount++] = i;
		unsigned r = c->start >> 24;
		if (r == 0x2 || r == 0x3) {  // writable RAM: mark the code bytes
			uint32_t size = region_size(r, gba);
			if (!rc_codeBits[r] && !(rc_codeBits[r] = calloc(size / 8, 1))) {
				return false;
			}
			for (uint32_t a = c->start & 0xFFFFFF; a < (c->end & 0xFFFFFF) && a < size; ++a) {
				rc_codeBits[r][a >> 3] |= (uint8_t) (1u << (a & 7));
			}
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
	size_t idx = (size_t) (c - recomp_chunks);
	if (s_validGen[idx] == rc_codeGen && !s_noGuardCache) {
		return true;
	}
	const uint8_t* live = region_live(c->start >> 24, s_gba);
	if (live && !memcmp(live + (c->start & 0xFFFFFF), c->guard, c->guardLen)) {
		s_validGen[idx] = rc_codeGen;
		return true;
	}
	return false;
}

static inline void ram_write(uint32_t a, uint32_t align, uint32_t size) {
	unsigned r = a >> 24;
	if (r == 0x2 || r == 0x3) {
		rc_code_write(r, a & align & (region_size(r, s_gba) - 1), size);
	}
}

// mGBA's own writes (DMA, BIOS decompression, the interpreter, our slow paths) go through these.
static struct ARMMemory s_mem;  // mGBA's original handlers
static struct ARMInterruptHandler s_irqh;

static void w_store32(struct ARMCore* cpu, uint32_t a, int32_t v, int* cc) {
	ram_write(a, 0xFFFFFC, 4);
	s_mem.store32(cpu, a, v, cc);
}
static void w_store16(struct ARMCore* cpu, uint32_t a, int16_t v, int* cc) {
	ram_write(a, 0xFFFFFE, 2);
	s_mem.store16(cpu, a, v, cc);
}
static void w_store8(struct ARMCore* cpu, uint32_t a, int8_t v, int* cc) {
	ram_write(a, 0xFFFFFF, 1);
	s_mem.store8(cpu, a, v, cc);
}
#ifdef RC_STATS
static uint64_t s_statStm[16];
#endif
static uint32_t w_storeMultiple(struct ARMCore* cpu, uint32_t base, int mask, enum LSMDirection dir, int* cc) {
	unsigned r = base >> 24;
#ifdef RC_STATS
	++s_statStm[r & 15];
#endif
	if (rc_codeBits[r & 15] && (r == 0x2 || r == 0x3)) {
		// At most 16 words on either side of the base address.
		uint32_t size = region_size(r, s_gba);
		for (int32_t d = -64; d < 68; d += 4) {
			uint32_t off = (uint32_t) (((int32_t) (base & 0xFFFFFC) + d)) & (size - 1);
			rc_code_write(r, off, 4);
		}
	}
	return s_mem.storeMultiple(cpu, base, mask, dir, cc);
}
static void w_swi16(struct ARMCore* cpu, int imm) {
	s_irqh.swi16(cpu, imm);
	if ((imm & 0xFF) <= 1) {  // SoftReset / RegisterRamReset clear RAM without going through stores
		++rc_codeGen;
	}
}
static void w_swi32(struct ARMCore* cpu, int imm) {
	s_irqh.swi32(cpu, imm);
	if (((imm >> 16) & 0xFF) <= 1) {
		++rc_codeGen;
	}
}

#ifdef RC_STATS
uint64_t rc_statInsns, rc_statSlowLoad[16], rc_statSlowStore[16];
uint64_t rc_statChunk[8192];
static uint64_t s_statActive, s_statLdm[16];
static void (*s_setActive)(struct ARMCore*, uint32_t);
static uint32_t (*s_ldm)(struct ARMCore*, uint32_t, int, enum LSMDirection, int*);
static void st_setActive(struct ARMCore* cpu, uint32_t a) {
	++s_statActive;
	s_setActive(cpu, a);
}
static uint32_t st_ldm(struct ARMCore* cpu, uint32_t b, int m, enum LSMDirection d, int* cc) {
	++s_statLdm[(b >> 24) & 15];
	return s_ldm(cpu, b, m, d, cc);
}
static void stats_hook(struct ARMCore* cpu) {
	s_setActive = cpu->memory.setActiveRegion;
	cpu->memory.setActiveRegion = st_setActive;
	s_ldm = cpu->memory.loadMultiple;
	cpu->memory.loadMultiple = st_ldm;
}
void recomp_stats_report(int frames) {
	fprintf(stderr, "stats/frame: insns %.0f  dispatch %.0f  setActiveRegion %.0f\n", rc_statInsns / (double) frames,
	        s_stats.nativeRuns / (double) frames, s_statActive / (double) frames);
	for (int k = 0; k < 15; ++k) {  // hottest chunks
		int best = -1;
		for (int i = 0; i < recomp_chunk_count && i < 8192; ++i) {
			if (rc_statChunk[i] && (best < 0 || rc_statChunk[i] > rc_statChunk[best])) best = i;
		}
		if (best < 0) break;
		fprintf(stderr, "  hot %08X-%08X %s %.0f insns/frame\n", recomp_chunks[best].start, recomp_chunks[best].end,
		        recomp_chunks[best].thumb ? "T" : "A", rc_statChunk[best] / (double) frames);
		rc_statChunk[best] = 0;
	}
	for (int r = 0; r < 16; ++r) {
		if (rc_statSlowLoad[r] || rc_statSlowStore[r] || s_statLdm[r] || s_statStm[r]) {
			fprintf(stderr, "  region %X: slow loads %.0f  slow stores %.0f  ldm %.0f  stm %.0f\n", r,
			        rc_statSlowLoad[r] / (double) frames, rc_statSlowStore[r] / (double) frames,
			        s_statLdm[r] / (double) frames, s_statStm[r] / (double) frames);
		}
	}
}
#endif

void recomp_invalidate(void) {
	++rc_codeGen;
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
	if (cpu->memory.store32 != w_store32) {
		s_mem = cpu->memory;
		cpu->memory.store32 = w_store32;
		cpu->memory.store16 = w_store16;
		cpu->memory.store8 = w_store8;
		cpu->memory.storeMultiple = w_storeMultiple;
		s_irqh = cpu->irqh;
#ifdef RC_STATS
		stats_hook(cpu);
#endif
		cpu->irqh.swi16 = w_swi16;
		cpu->irqh.swi32 = w_swi32;
	}
	if (cpu->irqh.processEvents != timed_process_events) {
		s_processEvents = cpu->irqh.processEvents;
		cpu->irqh.processEvents = timed_process_events;
	}
	core->runLoop = run_loop_core;
	memset(&s_stats, 0, sizeof(s_stats));
	memset(s_missKey, 0, sizeof(s_missKey));
	s_noGuardCache = getenv("RC_NOCACHE") != NULL;
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
