// Minimal GBA OBJ decoder: renders the current OAM into a transparent RGBA layer.
// Used to lift the HUD off the 3D scene so it can be re-laid out natively (curved on round screens).
#include "sprites.h"

#include <string.h>

#include <mgba/internal/gba/gba.h>

static const uint8_t SW[3][4] = { { 8, 16, 32, 64 }, { 16, 32, 32, 64 }, { 8, 8, 16, 32 } };
static const uint8_t SH[3][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 } };

static inline uint32_t bgr555_to_rgba(uint16_t c, uint8_t a) {
	uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
	r = (r << 3) | (r >> 2);
	g = (g << 3) | (g >> 2);
	b = (b << 3) | (b >> 2);
	return r | (g << 8) | (b << 16) | ((uint32_t) a << 24);
}

void sprites_decode(void* board, SpriteClassifyFn classify, uint32_t* const* layers, int nLayers) {
	struct GBA* gba = board;
	for (int l = 0; l < nLayers; ++l) {
		memset(layers[l], 0, SPR_W * SPR_H * sizeof(uint32_t));
	}

	uint16_t dispcnt = gba->memory.io[0];
	bool map1d = dispcnt & 0x40;
	bool bitmapMode = (dispcnt & 7) >= 3;
	const uint8_t* vram = (const uint8_t*) gba->video.vram;
	const uint16_t* pal = gba->video.palette + 256;
	const uint16_t* oam = gba->video.oam.raw;

	// Lower OAM index = higher priority, so paint from the back.
	for (int i = 127; i >= 0; --i) {
		uint16_t a = oam[i * 4], b = oam[i * 4 + 1], c = oam[i * 4 + 2];
		bool aff = a & 0x100;
		if (!aff && (a & 0x200)) {
			continue;  // hidden
		}
		bool dbl = aff && (a & 0x200);
		int mode = (a >> 10) & 3;
		int shape = a >> 14;
		if (mode >= 2 || shape == 3) {
			continue;  // OBJ window / invalid
		}
		int size = b >> 14;
		int w = SW[shape][size], h = SH[shape][size];
		int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
		int y = a & 0xFF, x = b & 0x1FF;
		if (y >= SPR_H) {
			y -= 256;
		}
		if (x >= SPR_W) {
			x -= 512;
		}
		if (x + bw <= 0 || y + bh <= 0) {
			continue;
		}
		bool bpp8 = a & 0x2000;
		int tile = c & 0x3FF;
		if (bitmapMode && tile < 512) {
			continue;  // that OBJ VRAM is used by the bitmap in modes 3-5
		}
		int palBank = c >> 12;
		int pa = 256, pb = 0, pc = 0, pd = 256;
		if (aff) {
			int m = (b >> 9) & 0x1F;
			pa = (int16_t) oam[m * 16 + 3];
			pb = (int16_t) oam[m * 16 + 7];
			pc = (int16_t) oam[m * 16 + 11];
			pd = (int16_t) oam[m * 16 + 15];
		}
		int layer = classify ? classify(x, y, bw, bh, aff, palBank) : 0;
		if (layer < 0 || layer >= nLayers) {
			continue;
		}
		uint32_t* out = layers[layer];
		bool hf = !aff && (b & 0x1000), vf = !aff && (b & 0x2000);
		uint8_t alpha = mode == 1 ? 170 : 255;  // semi-transparent OBJ
		int unit = bpp8 ? 2 : 1;
		int rowStride = map1d ? (w >> 3) * unit : 32;

		for (int py = 0; py < bh; ++py) {
			int sy = y + py;
			if (sy < 0 || sy >= SPR_H) {
				continue;
			}
			for (int px = 0; px < bw; ++px) {
				int sx = x + px;
				if (sx < 0 || sx >= SPR_W) {
					continue;
				}
				int tx, ty;
				if (aff) {
					int dx = px - bw / 2, dy = py - bh / 2;
					tx = ((pa * dx + pb * dy) >> 8) + w / 2;
					ty = ((pc * dx + pd * dy) >> 8) + h / 2;
					if (tx < 0 || ty < 0 || tx >= w || ty >= h) {
						continue;
					}
				} else {
					tx = hf ? w - 1 - px : px;
					ty = vf ? h - 1 - py : py;
				}
				int t = tile + (ty >> 3) * rowStride + (tx >> 3) * unit;
				uint32_t addr = 0x10000 + (uint32_t) (t & 0x3FF) * 32;
				uint16_t color;
				if (bpp8) {
					addr += (ty & 7) * 8 + (tx & 7);
					if (addr >= 0x18000) continue;
					uint8_t idx = vram[addr];
					if (!idx) continue;
					color = pal[idx];
				} else {
					addr += (ty & 7) * 4 + ((tx & 7) >> 1);
					if (addr >= 0x18000) continue;
					uint8_t byte = vram[addr];
					uint8_t idx = (tx & 1) ? byte >> 4 : byte & 15;
					if (!idx) continue;
					color = pal[palBank * 16 + idx];
				}
				out[sy * SPR_W + sx] = bgr555_to_rgba(color, alpha);
			}
		}
	}
}
