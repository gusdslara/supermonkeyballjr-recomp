// Presentation: game image placement (round / rect screens) + touch overlay.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

// Overlay-only actions (outside the 10 GBA key bits).
enum {
	VIEW_MODE = 1 << 12,  // tap: cycle control mode / hold: recalibrate tilt
};


void view_layout(int w, int h, bool round);
uint32_t view_hit(float x, float y);

// Cycles the presentation: round = in-stage zoom on/off; rectangular = 3:2 / widescreen / zoom.
// Returns a label for a toast.
const char* view_cycle_fit(void);
void view_zoom(float delta);  // rotary crown / wheel
void view_set_overlay(bool on);
bool view_overlay(void);
void view_set_mask(bool on);  // draw circular mask (desktop round simulation)
void view_set_smooth(bool on);
bool view_smooth(void);

void view_toast(const char* msg);
// Decoded stage HUD layers (HUD_COUNT x RGBA 240x160, see hud.h) or NULL outside stages.
// Round screens re-lay them out natively along the rim.
void view_set_hud_layers(const uint32_t* const* layers);
void view_set_debug(const char* text);  // NULL hides the debug line

void view_render(SDL_Renderer* r, SDL_Texture* game);
