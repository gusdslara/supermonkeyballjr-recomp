#pragma once
#include <stdbool.h>
#include <stdint.h>

// Stage HUD groups (Super Monkey Ball Jr. USA), used to re-lay the HUD out on round screens.
enum {
	HUD_MID = 0,   // GO!, GOAL!, results... -> follows the scene lens
	HUD_RIBBON,    // timer bomb, digits, TIME, fuse + spark -> bent along the top rim
	HUD_SCORE,     // SCORE label + score digits -> upright, upper-left
	HUD_LIVES,     // monkey heads (lives) -> upright, under the score
	HUD_BANANA,    // BANANA(S) label + count -> upright, upper-right
	HUD_FLOOR,     // stage badge + FLOOR nn -> upright, lower-left
	HUD_COUNT
};

// Nominal source rects (GBA px) of the upright groups, used for stable placement,
// and the rim direction (degrees, y-down) each one is pushed towards.
extern const float HUD_GROUP_RECT[HUD_COUNT][4];
extern const float HUD_GROUP_DIR[HUD_COUNT];

// True when the stage HUD sprites are in OAM (512 halfwords).
bool hud_detect_oam(const uint16_t* oam);

int hud_classify(int x, int y, int w, int h, bool affine, int palette);

// fb: 240x160, bytes R,G,B,X/A. useAlpha: treat alpha==0 pixels as empty (sprite layer).
// True while the in-stage HUD (static "SCORE" label) is on screen.
bool hud_detect(const uint32_t* fb, bool useAlpha);
