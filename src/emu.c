#include "emu.h"
#include "recomp/recomp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>

#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "hud.h"
#include "sprites.h"
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>

static struct mCore* s_core;
static uint32_t* s_fb;
static struct VFile* s_saveVf;

// HUD lifting: at the start of every frame (after the game's vblank OAM update, before line 0
// is drawn) decode the sprites; during stages they are all HUD, so hide the OBJ layer and let
// the frontend draw the decoded layer itself.
static bool s_hudSplit;
static bool s_hudActive;
static uint32_t s_hudLayers[HUD_COUNT][SPR_W * SPR_H];
static uint32_t* s_hudPtrs[HUD_COUNT];
static struct mCoreCallbacks s_callbacks;

static void on_frame_started(void* ctx) {
	if (!s_hudSplit) {
		return;
	}
	if (!s_hudPtrs[0]) {
		for (int i = 0; i < HUD_COUNT; ++i) {
			s_hudPtrs[i] = s_hudLayers[i];
		}
	}
	sprites_decode(s_core->board, hud_classify, s_hudPtrs, HUD_COUNT);
	// Hysteresis: switch on at once, switch off only after a sustained absence, so a
	// one-frame OAM hiccup never flips the layout back and forth.
	static int misses;
	struct GBA* gba = s_core->board;
	if (hud_detect_oam(gba->video.oam.raw)) {
		misses = 0;
		s_hudActive = true;
	} else if (++misses > 20) {
		s_hudActive = false;
	}
	s_core->enableVideoLayer(s_core, GBA_LAYER_OBJ, !s_hudActive);
}

void emu_set_hud_split(bool on) {
	s_hudSplit = on;
	if (!on && s_core) {
		s_hudActive = false;
		s_core->enableVideoLayer(s_core, GBA_LAYER_OBJ, true);
	}
}

bool emu_hud_active(void) { return s_hudActive; }
const uint32_t* const* emu_hud_layers(void) { return (const uint32_t* const*) s_hudPtrs; }

static void quiet_log(struct mLogger* l, int cat, enum mLogLevel level, const char* fmt, va_list args) {
	if (level & (mLOG_FATAL | mLOG_ERROR)) {
		char buf[512];
		vsnprintf(buf, sizeof(buf), fmt, args);
		SDL_Log("[mgba] %s", buf);
	}
}
static struct mLogger s_logger = { .log = quiet_log };

bool emu_init(const char* romPath, const char* savePath, char* err, int errLen) {
	mLogSetDefaultLogger(&s_logger);

	struct VFile* rom = VFileOpen(romPath, O_RDONLY);
	if (!rom) {
		snprintf(err, errLen, "ROM nao encontrada:\n%s", romPath);
		return false;
	}
	s_core = mCoreFindVF(rom);
	if (!s_core) {
		rom->close(rom);
		snprintf(err, errLen, "Arquivo nao e uma ROM de GBA valida");
		return false;
	}
	mCoreInitConfig(s_core, NULL);
	s_core->init(s_core);

	struct mCoreOptions opts = {
		.useBios = false,
		.skipBios = true,
		.volume = 0x100,
		.sampleRate = EMU_AUDIO_RATE,
	};
	mCoreConfigLoadDefaults(&s_core->config, &opts);
	mCoreConfigSetDefaultValue(&s_core->config, "idleOptimization", "detect");
	mCoreLoadConfig(s_core);

	s_fb = calloc(EMU_W * EMU_H, sizeof(uint32_t));
	s_core->setVideoBuffer(s_core, (color_t*) s_fb, EMU_W);

	// ~1 frame of audio per runFrame at 48 kHz is ~804 samples; give headroom.
	s_core->setAudioBufferSize(s_core, 2048);
	blip_set_rates(s_core->getAudioChannel(s_core, 0), s_core->frequency(s_core), EMU_AUDIO_RATE);
	blip_set_rates(s_core->getAudioChannel(s_core, 1), s_core->frequency(s_core), EMU_AUDIO_RATE);

	if (!s_core->loadROM(s_core, rom)) {
		snprintf(err, errLen, "Falha ao carregar ROM");
		return false;
	}

	if (savePath) {
		s_saveVf = VFileOpen(savePath, O_CREAT | O_RDWR);
		if (s_saveVf && !s_core->loadSave(s_core, s_saveVf)) {
			s_saveVf->close(s_saveVf);
			s_saveVf = NULL;
		}
		if (!s_saveVf) {
			SDL_Log("Aviso: save nao pode ser aberto em %s", savePath);
		}
	}

	memset(&s_callbacks, 0, sizeof(s_callbacks));
	s_callbacks.videoFrameStarted = on_frame_started;
	s_core->addCoreCallbacks(s_core, &s_callbacks);

	s_core->reset(s_core);
	if (recomp_attach(s_core)) {
		struct RecompStats st;
		recomp_stats(&st);
		SDL_Log("recomp: %d native instructions in %d chunks", st.insnCount, st.chunkCount);
	}
	return true;
}

void emu_shutdown(void) {
	if (!s_core) {
		return;
	}
	// Destroying the core flushes and closes the savedata VFile.
	s_core->deinit(s_core);
	s_core = NULL;
	s_saveVf = NULL;
	free(s_fb);
	s_fb = NULL;
}

void emu_reset(void) {
	if (s_core) {
		s_core->reset(s_core);
		recomp_invalidate();
	}
}

// Emulated-frame timing (CPU work only, no rendering), logged every PERF_FRAMES frames. On Android,
// `adb shell setprop debug.smbjr.ab 1` alternates native code and the interpreter at each report.
enum { PERF_FRAMES = 600 };
static uint64_t s_perfNs;
static int s_perfFrames;

static bool perf_ab(void) {
#ifdef __ANDROID__
	char v[PROP_VALUE_MAX] = { 0 };
	__system_property_get("debug.smbjr.ab", v);
	return v[0] == '1';
#else
	return SDL_GetHintBoolean("SMBJR_AB", false);
#endif
}

void emu_run_frame(uint16_t keys) {
	s_core->setKeys(s_core, keys);
	uint64_t t0 = SDL_GetTicksNS();
	s_core->runFrame(s_core);
	s_perfNs += SDL_GetTicksNS() - t0;
	if (++s_perfFrames == PERF_FRAMES) {
		double hw = recomp_take_hw_ns() / 1e6 / s_perfFrames;
		double all = s_perfNs / 1e6 / s_perfFrames;
		SDL_Log("perf: %s %.3f ms/frame = cpu %.3f + hardware %.3f (%d frames)",
		        recomp_enabled() ? "native" : "interpreter", all, all - hw, hw, s_perfFrames);
		s_perfNs = 0;
		s_perfFrames = 0;
		if (recomp_available() && perf_ab()) {
			recomp_set_enabled(!recomp_enabled());
		}
	}
}

const uint32_t* emu_framebuffer(void) {
	return s_fb;
}

int emu_read_audio(int16_t* out, int maxFrames) {
	blip_t* l = s_core->getAudioChannel(s_core, 0);
	blip_t* r = s_core->getAudioChannel(s_core, 1);
	int avail = blip_samples_avail(l);
	if (avail > maxFrames) {
		avail = maxFrames;
	}
	blip_read_samples(l, out, avail, true);
	blip_read_samples(r, out + 1, avail, true);
	return avail;
}

double emu_fps(void) {
	return (double) s_core->frequency(s_core) / (double) s_core->frameCycles(s_core);
}

bool emu_save_state(const char* path) {
	size_t size = s_core->stateSize(s_core);
	void* buf = malloc(size);
	bool ok = s_core->saveState(s_core, buf) && SDL_SaveFile(path, buf, size);
	free(buf);
	return ok;
}

bool emu_load_state(const char* path) {
	size_t size = 0;
	void* buf = SDL_LoadFile(path, &size);
	if (!buf) {
		return false;
	}
	bool ok = size == s_core->stateSize(s_core) && s_core->loadState(s_core, buf);
	recomp_invalidate();
	SDL_free(buf);
	return ok;
}

uint16_t emu_dispcnt(void) {
	return (uint16_t) s_core->busRead16(s_core, 0x04000000);
}


void emu_dump_oam(void) {
	struct GBA* gba = s_core->board;
	union GBAOAM* oam = &gba->video.oam;
	static const int SW[4][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
	static const int SH[4][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };
	for (int i = 0; i < 128; ++i) {
		uint16_t a = oam->raw[i * 4], b = oam->raw[i * 4 + 1], c = oam->raw[i * 4 + 2];
		bool aff = a & 0x100;
		if (!aff && (a & 0x200)) {
			continue;  // disabled
		}
		int shape = a >> 14, size = b >> 14;
		int y = a & 0xFF, x = b & 0x1FF;
		if (y >= 160) y -= 256;
		if (x >= 240) x -= 512;
		SDL_Log("obj%3d x=%4d y=%4d %dx%d aff=%d dbl=%d mode=%d 8bpp=%d tile=%4d prio=%d pal=%2d flip=%d%d",
		        i, x, y, SW[shape][size], SH[shape][size], aff, aff && (a & 0x200), (a >> 10) & 3, (a >> 13) & 1,
		        c & 0x3FF, (c >> 10) & 3, c >> 12, aff ? 0 : (b >> 12) & 1, aff ? 0 : (b >> 13) & 1);
	}
}

uint64_t emu_state_hash(void) {
	// Frame picture + work RAM: equal hashes mean the native and interpreted runs did the same thing.
	struct GBA* gba = s_core->board;
	uint64_t h = 0xcbf29ce484222325ULL;
	const uint8_t* parts[3] = { (const uint8_t*) s_fb, (const uint8_t*) gba->memory.wram, (const uint8_t*) gba->memory.iwram };
	size_t sizes[3] = { EMU_W * EMU_H * 4, 0x40000, 0x8000 };
	for (int p = 0; p < 3; ++p) {
		for (size_t i = 0; i < sizes[p]; ++i) {
			h ^= parts[p][i];
			h *= 0x100000001b3ULL;
		}
	}
	return h;
}
