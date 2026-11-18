// Gameplay HUD detection for Super Monkey Ball Jr. (USA).
// The static "SCORE" label is present only while a stage is being played; we compare its
// letter pixels (white fill / black outline), ignoring the gaps where the sky shows through.
#include "hud.h"

const float HUD_GROUP_RECT[HUD_COUNT][4] = {
	[HUD_SCORE] = { 5, 4, 92, 11 },
	[HUD_LIVES] = { 0, 14, 72, 22 },
	[HUD_BANANA] = { 156, 3, 80, 11 },
	[HUD_FLOOR] = { 0, 139, 80, 21 },
};

const float HUD_GROUP_DIR[HUD_COUNT] = {
	[HUD_SCORE] = -146,
	[HUD_LIVES] = -168,
	[HUD_BANANA] = -34,
	[HUD_FLOOR] = 132,
};

// Stage HUD signature in OAM: the 32x8 "SCORE" label at (7,6) and the 32x8 "FLOOR" label at
// (23,145). Position based, so palette flashes or VRAM tile moves don't matter.
bool hud_detect_oam(const uint16_t* oam) {
	bool score = false, floor = false;
	for (int i = 0; i < 128; ++i) {
		uint16_t a = oam[i * 4], b = oam[i * 4 + 1];
		if ((a & 0x300) == 0x200 || (a & 0x100)) {
			continue;  // hidden or affine
		}
		bool wide32x8 = (a >> 14) == 1 && (b >> 14) == 1;
		if (!wide32x8) {
			continue;
		}
		int y = a & 0xFF, x = b & 0x1FF;
		score |= x == 7 && y == 6;
		floor |= x == 23 && y == 145;
	}
	return score && floor;
}

int hud_classify(int x, int y, int w, int h, bool affine, int palette) {
	int cx = x + w / 2, cy = y + h / 2;
	if (cy >= 120 && cx < 120) {
		return HUD_FLOOR;
	}
	if (cy < 44) {
		if (palette == 1 && !affine && x >= 150) {
			return HUD_BANANA;
		}
		if (x + w <= 100) {
			return affine ? HUD_LIVES : HUD_SCORE;
		}
		return HUD_RIBBON;
	}
	return HUD_MID;
}

#define SIG_X 6
#define SIG_Y 4
#define SIG_W 39
#define SIG_H 11

static const char* const SIG[SIG_H] = {
	".......................................",
	".......................................",
	"..KKKKKK.KKKKKKKKKKKKKKKKKKKKKKKKKKKK..",
	".K.WWWWWK.WWWWWK.WWWW.KWWWWW.KWWWWWWK..",
	".KWWKKKKKWWKKKKKWWKKWWKWWKKWWKWWKKKKK..",
	".K.WWWW.KWWK...KWWKKWWKWWWWW.KWWWWWWK..",
	".KKKKKWWKWWKKKKKWWKKWWKWWKKWWKWWKKKKK..",
	".KWWWWW.K.WWWWWK.WWWW.KWWKKWWKWWWWWWK..",
	"..KKKKKK.KKKKKKKKKKKKKKKK..KKKKKKKKKK..",
	".......................................",
	".......................................",
};

bool hud_detect(const uint32_t* fb, bool useAlpha) {
	int total = 0, match = 0;
	for (int y = 0; y < SIG_H; ++y) {
		for (int x = 0; x < SIG_W; ++x) {
			char want = SIG[y][x];
			if (want == '.') {
				continue;
			}
			uint32_t p = fb[(SIG_Y + y) * 240 + SIG_X + x];  // XBGR8888
			int r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
			int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
			int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
			char got = mn > 200 ? 'W' : (mx < 50 ? 'K' : '.');
			if (useAlpha && (p >> 24) == 0) {
				got = '.';
			}
			++total;
			match += got == want;
		}
	}
	return total && match * 100 >= total * 85;
}
