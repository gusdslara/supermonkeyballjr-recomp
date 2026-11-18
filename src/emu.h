// Thin wrapper around libmgba: ROM load, frame stepping, audio drain, EEPROM save.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define EMU_W 240
#define EMU_H 160
#define EMU_AUDIO_RATE 48000

// GBA key bits (same order as mGBA's enum GBAKey)
enum {
	KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3,
	KEY_RIGHT = 1 << 4, KEY_LEFT = 1 << 5, KEY_UP = 1 << 6, KEY_DOWN = 1 << 7,
	KEY_R = 1 << 8, KEY_L = 1 << 9,
};

bool emu_init(const char* romPath, const char* savePath, char* err, int errLen);
void emu_shutdown(void);
void emu_reset(void);

// Runs one emulated frame with the given key mask.
void emu_run_frame(uint16_t keys);

// XBGR8888 framebuffer, EMU_W x EMU_H, stride EMU_W pixels.
const uint32_t* emu_framebuffer(void);

// Reads up to maxFrames interleaved stereo s16 frames; returns frames read.
int emu_read_audio(int16_t* out, int maxFrames);

// Emulated frames per second (~59.7275).
double emu_fps(void);

// Save-state slot helpers (desktop/dev convenience).
bool emu_save_state(const char* path);
bool emu_load_state(const char* path);

// Debug: current display mode (DISPCNT & 7) and raw DISPCNT.
uint16_t emu_dispcnt(void);
void emu_dump_oam(void);

// Round-screen HUD lifting. When enabled and a stage HUD is detected in OAM, the OBJ layer is
// removed from the framebuffer and provided separately as RGBA layers, one per HUD_* group
// (240x160 each, alpha 0 = empty).
void emu_set_hud_split(bool on);
bool emu_hud_active(void);
const uint32_t* const* emu_hud_layers(void);

// Hash of the framebuffer and work RAM (native vs. interpreter comparison).
uint64_t emu_state_hash(void);
