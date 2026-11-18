// Unified input: touch trail-stick, device/gamepad tilt, gamepads, keyboard.
// Everything is merged into one analog vector, then converted to GBA D-pad
// presses with sigma-delta PWM so tilt intensity maps to ball acceleration.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

typedef enum {
	CTRL_TOUCH = 0,  // finger trail-stick (gen1recomp style)
	CTRL_TILT,       // tilt only; touches only hit buttons
	CTRL_HYBRID,     // tilt + finger stick summed
	CTRL_COUNT
} ControlMode;

// Hit-test callback: returns GBA key bits / VIEW_* bits for a pixel position.
typedef uint32_t (*InputHitFn)(float x, float y);

typedef struct {
	bool active;
	float ax, ay;  // anchor (px)
	float x, y;    // current finger (px)
	float radius;  // px
} TouchStick;

void input_init(void);
void input_shutdown(void);
void input_set_hit_fn(InputHitFn fn);
void input_set_screen(int w, int h);

// Feed every SDL event. Returns special VIEW_* bits pressed this event (edge).
uint32_t input_handle_event(const SDL_Event* e);

// Call once per emulated frame. Returns GBA key mask.
uint16_t input_frame(void);

// Held touch-overlay bits (GBA keys + VIEW_* bits) for drawing pressed state.
uint32_t input_touch_held(void);
const TouchStick* input_stick(void);
void input_analog(float* x, float* y);  // last merged analog value

ControlMode input_mode(void);
void input_set_mode(ControlMode m);
const char* input_mode_name(ControlMode m);
bool input_has_tilt(void);
// Sets the tilt "neutral" a moment later (the hand leaves the screen first): the pose the
// watch is held in then, e.g. raised like checking the time, becomes "no tilt".
void input_calibrate(void);
// Same, silently: done at every stage start so the neutral follows how the watch is held.
void input_calibrate_quiet(void);
// M button / F1: toggles finger <-> gyro (gyro also takes the finger stick). Returns the toast.
const char* input_cycle_mode(void);
void input_set_pwm(bool on);
bool input_pwm(void);
bool input_touch_seen(void);  // a real (non-mouse) touch happened
// Touch overlay visible: last input was a finger (or the platform defaults to touch).
bool input_touch_mode(void);
void input_set_default_touch(bool on);
// Holds `keys` (GBA key bits) for `frames` emulated frames (automation/testing).
void input_debug_press(uint16_t keys, int frames);
