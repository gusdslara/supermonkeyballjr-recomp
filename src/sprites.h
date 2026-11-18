#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SPR_W 240
#define SPR_H 160

// Picks the output layer for one sprite (screen bbox in GBA px). Return <0 to drop it.
typedef int (*SpriteClassifyFn)(int x, int y, int w, int h, bool affine, int palette);

// Renders all visible OBJs of `board` (struct GBA*) into `layers[classify(...)]`
// (RGBA8888 bytes R,G,B,A; alpha 0 = empty). All `nLayers` layers are cleared first.
void sprites_decode(void* board, SpriteClassifyFn classify, uint32_t* const* layers, int nLayers);
