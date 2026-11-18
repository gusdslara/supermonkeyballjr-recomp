#include "input.h"
#include "emu.h"
#include "view.h"

#include <math.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define MAX_FINGERS 10
#define MAX_PADS 4
#define MODE_HOLD_NS (600ull * 1000000ull)

#define TILT_MAX_DEG 22.0f
#define TILT_DEADZONE 0.08f
#define STICK_DEADZONE 0.12f
#define PAD_DEADZONE 0.20f

// Orientation estimate: unit "up" vector in the sensor frame. The gyroscope rotates it at the
// sensor rate (instant, follows the real wrist/hand rotation, ignores shakes); the accelerometer
// pulls it slowly back to gravity so it never drifts. Without a gyroscope it falls back to a
// short low-pass on the accelerometer.
typedef struct {
	bool valid;
	float g[3];
	uint64_t accTs, gyroTs;   // last sample time (ns)
	uint64_t gyroAliveNs;     // last time the gyroscope reported a real (non-zero) rate
} Fusion;

typedef struct {
	bool used;
	SDL_FingerID id;
	SDL_TouchID touch;
	bool isStick;
	uint32_t bits;
} Finger;

static struct {
	ControlMode mode;
	bool pwm;
	int w, h;
	InputHitFn hit;

	Finger fingers[MAX_FINGERS];
	TouchStick stick;
	SDL_FingerID stickId;
	bool touchSeen;
	bool defaultTouch;     // platform starts with the touch overlay (phones, watches, touch web)
	uint64_t lastTouchNs;  // last real finger
	uint64_t lastPadNs;    // last gamepad / keyboard activity

	uint64_t modeDownNs;
	bool modeHeld, modeHoldFired;

	SDL_Gamepad* pads[MAX_PADS];
	SDL_Sensor* accel;
	SDL_Sensor* gyro;

	Fusion dev;           // device accelerometer + gyroscope
	Fusion pad;           // gamepad motion sensors (DualSense, Switch Pro...)
	bool calibrated;
	bool calibFromPad;
	float n0[3];          // neutral "up" (sensor frame), taken at calibration
	int basisOrient;      // screen orientation the basis was built for
	float ex[3], ey[3];
	bool calibPending;
	bool calibQuiet;      // no toast (automatic, at stage start)
	uint64_t calibAtNs;   // pending calibration happens at this time (0 = now)

	int startPulse;    // frames left of a synthetic START press (Android back button)
	uint16_t debugKeys;  // scripted presses (web automation hook)
	int debugFrames;
	uint32_t tapLatch; // button bits released before the game could see them
	int tapFrames;

	float ax, ay;      // last merged analog
	float accX, accY;  // sigma-delta accumulators
} S;

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : v > hi ? hi : v;
}
static float dot3(const float* a, const float* b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void norm3(float* v) {
	float l = sqrtf(dot3(v, v));
	if (l > 1e-6f) {
		v[0] /= l; v[1] /= l; v[2] /= l;
	}
}

static void open_sensors(void) {
	int n = 0;
	SDL_SensorID* ids = SDL_GetSensors(&n);
	for (int i = 0; ids && i < n; ++i) {
		SDL_SensorType t = SDL_GetSensorTypeForID(ids[i]);
		if (t == SDL_SENSOR_ACCEL && !S.accel) {
			S.accel = SDL_OpenSensor(ids[i]);
			if (S.accel) {
				SDL_Log("Acelerometro: %s", SDL_GetSensorName(S.accel));
			}
		} else if (t == SDL_SENSOR_GYRO && !S.gyro) {
			S.gyro = SDL_OpenSensor(ids[i]);
			if (S.gyro) {
				SDL_Log("Giroscopio: %s", SDL_GetSensorName(S.gyro));
			}
		}
	}
	SDL_free(ids);
}

void input_init(void) {
	memset(&S, 0, sizeof(S));
	S.pwm = true;
	S.mode = CTRL_TOUCH;
	open_sensors();
}

void input_shutdown(void) {
	for (int i = 0; i < MAX_PADS; ++i) {
		if (S.pads[i]) {
			SDL_CloseGamepad(S.pads[i]);
		}
	}
	if (S.accel) {
		SDL_CloseSensor(S.accel);
	}
	if (S.gyro) {
		SDL_CloseSensor(S.gyro);
	}
}

void input_set_hit_fn(InputHitFn fn) { S.hit = fn; }

void input_set_screen(int w, int h) {
	S.w = w;
	S.h = h;
	S.stick.radius = 0.16f * (float) (w < h ? w : h);
}

ControlMode input_mode(void) { return S.mode; }
bool input_pwm(void) { return S.pwm; }
void input_set_pwm(bool on) { S.pwm = on; }
bool input_touch_seen(void) { return S.touchSeen; }
void input_set_default_touch(bool on) { S.defaultTouch = on; }

void input_debug_press(uint16_t keys, int frames) {
	S.debugKeys = keys;
	S.debugFrames = frames;
}

bool input_touch_mode(void) {
	if (S.lastPadNs > S.lastTouchNs) {
		return false;  // playing with a controller/keyboard: hide the touch overlay
	}
	return S.lastTouchNs != 0 || S.defaultTouch;
}
const TouchStick* input_stick(void) { return &S.stick; }
void input_analog(float* x, float* y) { *x = S.ax; *y = S.ay; }

const char* input_mode_name(ControlMode m) {
	switch (m) {
	case CTRL_TOUCH: return "Dedo";
	case CTRL_TILT: return "Giroscopio";
	case CTRL_HYBRID: return "Giroscopio";
	default: return "?";
	}
}

bool input_has_tilt(void) {
	if (S.accel) {
		return true;
	}
	for (int i = 0; i < MAX_PADS; ++i) {
		if (S.pads[i] && SDL_GamepadHasSensor(S.pads[i], SDL_SENSOR_ACCEL)) {
			return true;
		}
	}
	return false;
}

void input_set_mode(ControlMode m) {
	S.mode = m;
	S.stick.active = false;
	if (m != CTRL_TOUCH) {
		input_calibrate_quiet();
	}
}

// The finger that tapped M is still on its way back: take the neutral a little later.
#define CALIB_DELAY_NS (700ull * 1000000ull)

void input_calibrate(void) {
	S.calibPending = true;
	S.calibQuiet = false;
	S.calibAtNs = SDL_GetTicksNS() + CALIB_DELAY_NS;
}

void input_calibrate_quiet(void) {
	S.calibPending = true;
	S.calibQuiet = true;
	S.calibAtNs = SDL_GetTicksNS() + CALIB_DELAY_NS;
}

// Two modes only: finger, or gyro (which also keeps the finger stick for fine corrections).
const char* input_cycle_mode(void) {
	if (S.mode == CTRL_TOUCH && !input_has_tilt()) {
		return "Sem giroscopio";
	}
	input_set_mode(S.mode == CTRL_TOUCH ? CTRL_HYBRID : CTRL_TOUCH);
	return input_mode_name(S.mode);
}

// ------------------------------------------------------------------ tilt

#define GRAVITY 9.80665f
#define FUSE_TAU_GYRO 0.6f     // s: accelerometer correction time constant with a gyroscope
#define FUSE_TAU_ACCEL 0.05f   // s: plain low-pass without one
#define GYRO_ALIVE_NS (500ull * 1000000ull)

static void cross3(const float* a, const float* b, float* o) {
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

#ifdef __EMSCRIPTEN__
static bool web_is_ios(void) {
	static int ios = -1;
	if (ios < 0) {
		ios = EM_ASM_INT({ return /iPad|iPhone|iPod/.test(navigator.userAgent) ||
		                          (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1) ? 1 : 0; });
	}
	return ios != 0;
}
#endif

static void fusion_accel(Fusion* f, const float* d, uint64_t ts) {
	float a[3] = { d[0], d[1], d[2] };
	float n = sqrtf(dot3(a, a));
	if (n < 1.0f) {
		return;
	}
	a[0] /= n; a[1] /= n; a[2] /= n;
	float dt = (f->accTs && ts > f->accTs) ? (float) (ts - f->accTs) * 1e-9f : 0.016f;
	f->accTs = ts;
	if (!f->valid) {
		memcpy(f->g, a, sizeof(f->g));
		f->valid = true;
		return;
	}
	dt = clampf(dt, 0.001f, 0.1f);
	bool gyro = f->gyroAliveNs && SDL_GetTicksNS() - f->gyroAliveNs < GYRO_ALIVE_NS;
	float k = dt / ((gyro ? FUSE_TAU_GYRO : FUSE_TAU_ACCEL) + dt);
	if (gyro) {
		// Swinging the arm adds linear acceleration: trust the accelerometer less while |a| != 1 g.
		k *= clampf(1.0f - fabsf(n - GRAVITY) / (0.35f * GRAVITY), 0.0f, 1.0f);
	}
	for (int i = 0; i < 3; ++i) {
		f->g[i] += (a[i] - f->g[i]) * k;
	}
	norm3(f->g);
}

static void fusion_gyro(Fusion* f, const float* w, uint64_t ts) {
	float dt = (f->gyroTs && ts > f->gyroTs) ? (float) (ts - f->gyroTs) * 1e-9f : 0.0f;
	f->gyroTs = ts;
	if (w[0] != 0.0f || w[1] != 0.0f || w[2] != 0.0f) {
		f->gyroAliveNs = SDL_GetTicksNS();
	}
	if (!f->valid || dt <= 0.0f || dt > 0.1f) {
		return;
	}
	// A world-fixed vector seen from a frame rotating at w: dg/dt = g x w.
	float c[3];
	cross3(f->g, w, c);
	for (int i = 0; i < 3; ++i) {
		f->g[i] += c[i] * dt;
	}
	norm3(f->g);
}

static uint64_t sensor_ts(uint64_t sensorTs, uint64_t eventTs) {
	return sensorTs ? sensorTs : eventTs;
}

static void handle_device_sensor(const SDL_SensorEvent* se) {
	SDL_Sensor* s = SDL_GetSensorFromID(se->which);
	uint64_t ts = sensor_ts(se->sensor_timestamp, se->timestamp);
	if (s && s == S.accel) {
		float a[3] = { se->data[0], se->data[1], se->data[2] };
#ifdef __EMSCRIPTEN__
		if (web_is_ios()) {  // iOS reports accelerationIncludingGravity with the opposite sign
			a[0] = -a[0]; a[1] = -a[1]; a[2] = -a[2];
		}
#endif
		fusion_accel(&S.dev, a, ts);
	} else if (s && s == S.gyro) {
#ifdef __EMSCRIPTEN__
		// SDL's web backend stores rotationRate as (alpha, beta, gamma) = rates about (z, x, y).
		float w[3] = { se->data[1], se->data[2], se->data[0] };
#else
		float w[3] = { se->data[0], se->data[1], se->data[2] };
#endif
		fusion_gyro(&S.dev, w, ts);
	}
}

static int screen_orientation(bool fromPad) {
	if (fromPad) {
		return 0;
	}
#ifdef __EMSCRIPTEN__
	// Browsers report motion in the device's natural frame; ask the page for the screen angle.
	return EM_ASM_INT({
		var a = (screen.orientation && screen.orientation.angle !== undefined) ? screen.orientation.angle
		                                                                       : (window.orientation || 0);
		return ((a % 360) + 360) % 360;
	});
#else
	switch (SDL_GetCurrentDisplayOrientation(SDL_GetPrimaryDisplay())) {
	case SDL_ORIENTATION_LANDSCAPE: return 90;
	case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return 270;
	case SDL_ORIENTATION_PORTRAIT_FLIPPED: return 180;
	default: return 0;
	}
#endif
}

// Screen "right" and screen "up" (towards the top edge) in the sensor frame.
static void screen_axes(int orient, float* r, float* u) {
	r[0] = 1; r[1] = 0; r[2] = 0;
	u[0] = 0; u[1] = 1; u[2] = 0;
	switch (orient) {
	case 90:  r[0] = 0; r[1] = -1; u[0] = 1;  u[1] = 0; break;
	case 270: r[0] = 0; r[1] = 1;  u[0] = -1; u[1] = 0; break;
	case 180: r[0] = -1; u[1] = -1; break;
	default: break;
	}
}

// Basis on the plane normal to the neutral "up" n0: ex = screen right, ey = n0 x ex (towards
// the top edge). Built from both screen axes so it stays well defined in any pose: looking at
// the watch, wrist turned down, arm hanging (screen-right then points at the floor), phone flat
// or upright.
static void build_basis(int orient) {
	float r[3], u[3];
	screen_axes(orient, r, u);
	const float* n = S.n0;
	float a[3], b[3], pu[3];
	float dr = dot3(r, n), du = dot3(u, n);
	for (int i = 0; i < 3; ++i) {
		a[i] = r[i] - dr * n[i];
		pu[i] = u[i] - du * n[i];
	}
	cross3(pu, n, b);  // projected "up" turned into "right" (ey x n0 = ex)
	for (int i = 0; i < 3; ++i) {
		S.ex[i] = a[i] + b[i];  // each term weighs by how well-conditioned it is
	}
	if (dot3(S.ex, S.ex) < 1e-6f) {
		memcpy(S.ex, r, sizeof(S.ex));
	}
	norm3(S.ex);
	cross3(n, S.ex, S.ey);
	S.basisOrient = orient;
}

static const Fusion* tilt_source(bool* fromPad) {
	if (S.accel && S.dev.valid) {
		*fromPad = false;
		return &S.dev;
	}
	if (S.pad.valid) {
		*fromPad = true;
		return &S.pad;
	}
	return NULL;
}

static void tilt_vector(float* tx, float* ty) {
	*tx = *ty = 0;
	bool fromPad = false;
	const Fusion* f = tilt_source(&fromPad);
	if (!f) {
		return;
	}
	const float* gn = f->g;

	if (S.calibPending && S.calibAtNs && SDL_GetTicksNS() < S.calibAtNs) {
		return;  // neutral not taken yet: no tilt meanwhile
	}
	if (S.calibPending || !S.calibrated || S.calibFromPad != fromPad) {
		memcpy(S.n0, gn, sizeof(S.n0));
		S.calibFromPad = fromPad;
		build_basis(screen_orientation(fromPad));
		if (S.calibPending && S.calibrated && !S.calibQuiet) {
			view_toast("Calibrado");
		}
		S.calibrated = true;
		S.calibPending = false;
		S.calibQuiet = false;
		S.calibAtNs = 0;
	} else {
		// Phone turned to the other landscape side: same neutral pose, new screen axes.
		int o = screen_orientation(fromPad);
		if (o != S.basisOrient) {
			build_basis(o);
		}
	}

	const float k = 1.0f / sinf(TILT_MAX_DEG * 3.14159265f / 180.0f);
	float x = -dot3(gn, S.ex) * k;  // right edge down -> right
	float y = dot3(gn, S.ey) * k;   // top edge down (away) -> up (negative y)

	float m = sqrtf(x * x + y * y);
	if (m < TILT_DEADZONE) {
		return;
	}
	float scaled = (m - TILT_DEADZONE) / (1.0f - TILT_DEADZONE);
	*tx = x / m * scaled;
	*ty = y / m * scaled;
}

// ------------------------------------------------------------------ touch

static Finger* finger_find(SDL_TouchID t, SDL_FingerID f) {
	for (int i = 0; i < MAX_FINGERS; ++i) {
		if (S.fingers[i].used && S.fingers[i].id == f && S.fingers[i].touch == t) {
			return &S.fingers[i];
		}
	}
	return NULL;
}

static void finger_down(SDL_TouchID t, SDL_FingerID f, float x, float y) {
	Finger* fg = NULL;
	for (int i = 0; i < MAX_FINGERS; ++i) {
		if (!S.fingers[i].used) {
			fg = &S.fingers[i];
			break;
		}
	}
	if (!fg) {
		return;
	}
	memset(fg, 0, sizeof(*fg));
	fg->used = true;
	fg->id = f;
	fg->touch = t;

	uint32_t bits = S.hit ? S.hit(x, y) : 0;
	if (bits) {
		fg->bits = bits;
		if (bits & VIEW_MODE) {
			S.modeHeld = true;
			S.modeHoldFired = false;
			S.modeDownNs = SDL_GetTicksNS();
		}
		return;
	}
	if (S.mode != CTRL_TILT && !S.stick.active) {
		fg->isStick = true;
		S.stickId = f;
		S.stick.active = true;
		S.stick.ax = S.stick.x = x;
		S.stick.ay = S.stick.y = y;
	}
}

static void finger_motion(Finger* fg, float x, float y) {
	if (!fg->isStick) {
		return;
	}
	S.stick.x = x;
	S.stick.y = y;
	float dx = x - S.stick.ax, dy = y - S.stick.ay;
	float d = sqrtf(dx * dx + dy * dy);
	if (d > S.stick.radius) {
		// Trail: drag the anchor behind the thumb so direction changes are instant.
		S.stick.ax = x - dx / d * S.stick.radius;
		S.stick.ay = y - dy / d * S.stick.radius;
	}
}

static void finger_up(Finger* fg) {
	if (fg->isStick) {
		S.stick.active = false;
	}
	if (fg->bits & 0x3FF) {
		// Keep quick taps visible to the game for a few frames.
		S.tapLatch |= fg->bits & 0x3FF;
		S.tapFrames = 4;
	}
	if ((fg->bits & VIEW_MODE) && S.modeHeld) {
		S.modeHeld = false;
		if (!S.modeHoldFired) {
			view_toast(input_cycle_mode());
		}
	}
	fg->used = false;
}

uint32_t input_touch_held(void) {
	uint32_t bits = 0;
	for (int i = 0; i < MAX_FINGERS; ++i) {
		if (S.fingers[i].used) {
			bits |= S.fingers[i].bits;
		}
	}
	return bits;
}

// ------------------------------------------------------------------ events

uint32_t input_handle_event(const SDL_Event* e) {
	switch (e->type) {
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED: {
		float x = e->tfinger.x * (float) S.w;
		float y = e->tfinger.y * (float) S.h;
		if (e->tfinger.touchID != SDL_MOUSE_TOUCHID) {
			S.touchSeen = true;
			S.lastTouchNs = SDL_GetTicksNS();
		}
		if (e->type == SDL_EVENT_FINGER_DOWN) {
			finger_down(e->tfinger.touchID, e->tfinger.fingerID, x, y);
		} else {
			Finger* fg = finger_find(e->tfinger.touchID, e->tfinger.fingerID);
			if (!fg) {
				break;
			}
			if (e->type == SDL_EVENT_FINGER_MOTION) {
				finger_motion(fg, x, y);
			} else {
				finger_up(fg);
			}
		}
		break;
	}
	case SDL_EVENT_GAMEPAD_ADDED:
		for (int i = 0; i < MAX_PADS; ++i) {
			if (!S.pads[i]) {
				S.pads[i] = SDL_OpenGamepad(e->gdevice.which);
				if (S.pads[i]) {
					if (SDL_GamepadHasSensor(S.pads[i], SDL_SENSOR_ACCEL)) {
						SDL_SetGamepadSensorEnabled(S.pads[i], SDL_SENSOR_ACCEL, true);
					}
					if (SDL_GamepadHasSensor(S.pads[i], SDL_SENSOR_GYRO)) {
						SDL_SetGamepadSensorEnabled(S.pads[i], SDL_SENSOR_GYRO, true);
					}
					char buf[96];
					SDL_snprintf(buf, sizeof(buf), "Controle: %s", SDL_GetGamepadName(S.pads[i]));
					view_toast(buf);
				}
				break;
			}
		}
		break;
	case SDL_EVENT_GAMEPAD_REMOVED:
		for (int i = 0; i < MAX_PADS; ++i) {
			if (S.pads[i] && SDL_GetGamepadID(S.pads[i]) == e->gdevice.which) {
				SDL_CloseGamepad(S.pads[i]);
				S.pads[i] = NULL;
			}
		}
		break;
	case SDL_EVENT_SENSOR_UPDATE:
		handle_device_sensor(&e->sensor);
		break;
	case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
		uint64_t ts = sensor_ts(e->gsensor.sensor_timestamp, e->gsensor.timestamp);
		if (e->gsensor.sensor == SDL_SENSOR_ACCEL) {
			fusion_accel(&S.pad, e->gsensor.data, ts);
		} else if (e->gsensor.sensor == SDL_SENSOR_GYRO) {
			fusion_gyro(&S.pad, e->gsensor.data, ts);
		}
		break;
	}
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		if (e->gaxis.value > 16000 || e->gaxis.value < -16000) {
			S.lastPadNs = SDL_GetTicksNS();
		}
		break;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		S.lastPadNs = SDL_GetTicksNS();
		// Right-stick click: recalibrate tilt; Guide: cycle control mode.
		if (e->gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
			input_calibrate();
		} else if (e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
			ControlMode m = (ControlMode) ((S.mode + 1) % CTRL_COUNT);
			input_set_mode(m);
			char buf[64];
			SDL_snprintf(buf, sizeof(buf), "Controle: %s", input_mode_name(m));
			view_toast(buf);
		}
		break;
	case SDL_EVENT_KEY_DOWN:
		// Watch/phone back button (trapped by SDL) -> START, i.e. the pause menu.
		if (e->key.scancode == SDL_SCANCODE_AC_BACK && !e->key.repeat) {
			S.startPulse = 6;
		} else {
			S.lastPadNs = SDL_GetTicksNS();
		}
		break;
	default:
		break;
	}
	return 0;
}

// ------------------------------------------------------------------ per frame

static uint16_t pwm_axis(float v, float* acc, uint16_t negBit, uint16_t posBit) {
	float m = fabsf(v);
	if (m < 0.02f) {
		*acc = 0;
		return 0;
	}
	uint16_t bit = v < 0 ? negBit : posBit;
	if (!S.pwm) {
		return m > 0.35f ? bit : 0;
	}
	if (m >= 0.92f) {
		*acc = 0;
		return bit;
	}
	// Sigma-delta: press on a fraction of frames proportional to deflection.
	*acc += m;
	if (*acc >= 1.0f) {
		*acc -= 1.0f;
		return bit;
	}
	return 0;
}

uint16_t input_frame(void) {
	uint16_t keys = 0;

	// Long-press on MODE: recalibrate.
	if (S.modeHeld && !S.modeHoldFired && SDL_GetTicksNS() - S.modeDownNs > MODE_HOLD_NS) {
		S.modeHoldFired = true;
		S.calibrated = true;
		if (S.mode != CTRL_TOUCH) {
			input_calibrate();
		}
	}

	// Keyboard
	const bool* ks = SDL_GetKeyboardState(NULL);
	if (ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_W]) keys |= KEY_UP;
	if (ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_S]) keys |= KEY_DOWN;
	if (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A]) keys |= KEY_LEFT;
	if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D]) keys |= KEY_RIGHT;
	if (ks[SDL_SCANCODE_Z] || ks[SDL_SCANCODE_K] || ks[SDL_SCANCODE_SPACE]) keys |= KEY_A;
	if (ks[SDL_SCANCODE_X] || ks[SDL_SCANCODE_J]) keys |= KEY_B;
	if (ks[SDL_SCANCODE_RETURN]) keys |= KEY_START;
	if (ks[SDL_SCANCODE_BACKSPACE] || ks[SDL_SCANCODE_RSHIFT]) keys |= KEY_SELECT;
	if (ks[SDL_SCANCODE_Q]) keys |= KEY_L;
	if (ks[SDL_SCANCODE_E]) keys |= KEY_R;
	if (S.startPulse > 0) {
		--S.startPulse;
		keys |= KEY_START;
	}
	if (S.debugFrames > 0) {
		--S.debugFrames;
		keys |= S.debugKeys;
	}

	// Touch overlay buttons
	keys |= (uint16_t) (input_touch_held() & 0x3FF);
	if (S.tapFrames > 0) {
		keys |= (uint16_t) S.tapLatch;
		if (--S.tapFrames == 0) {
			S.tapLatch = 0;
		}
	}

	float ax = 0, ay = 0;

	// Gamepads
	for (int i = 0; i < MAX_PADS; ++i) {
		SDL_Gamepad* p = S.pads[i];
		if (!p) {
			continue;
		}
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH)) keys |= KEY_A;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST)) keys |= KEY_B;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START)) keys |= KEY_START;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_BACK)) keys |= KEY_SELECT;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) keys |= KEY_L;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) keys |= KEY_R;
		if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) keys |= KEY_L;
		if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) keys |= KEY_R;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP)) keys |= KEY_UP;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) keys |= KEY_DOWN;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) keys |= KEY_LEFT;
		if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) keys |= KEY_RIGHT;
		float sx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
		float sy = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
		float m = sqrtf(sx * sx + sy * sy);
		if (m > PAD_DEADZONE) {
			float k = clampf((m - PAD_DEADZONE) / (1.0f - PAD_DEADZONE), 0, 1) / m;
			ax += sx * k;
			ay += sy * k;
		}
	}

	// Finger trail-stick
	if (S.stick.active && S.mode != CTRL_TILT) {
		float dx = (S.stick.x - S.stick.ax) / S.stick.radius;
		float dy = (S.stick.y - S.stick.ay) / S.stick.radius;
		float m = sqrtf(dx * dx + dy * dy);
		if (m > STICK_DEADZONE) {
			float k = clampf((m - STICK_DEADZONE) / (1.0f - STICK_DEADZONE), 0, 1) / m;
			ax += dx * k;
			ay += dy * k;
		}
	}

	// Tilt (device accelerometer, or gamepad motion sensor)
	if (S.mode != CTRL_TOUCH) {
		float tx, ty;
		tilt_vector(&tx, &ty);
		ax += tx;
		ay += ty;
	}

	ax = clampf(ax, -1, 1);
	ay = clampf(ay, -1, 1);
	S.ax = ax;
	S.ay = ay;
	keys |= pwm_axis(ax, &S.accX, KEY_LEFT, KEY_RIGHT);
	keys |= pwm_axis(ay, &S.accY, KEY_UP, KEY_DOWN);

	// Never send opposing directions (keyboard + analog conflicts).
	if ((keys & (KEY_LEFT | KEY_RIGHT)) == (KEY_LEFT | KEY_RIGHT)) keys &= ~(KEY_LEFT | KEY_RIGHT);
	if ((keys & (KEY_UP | KEY_DOWN)) == (KEY_UP | KEY_DOWN)) keys &= ~(KEY_UP | KEY_DOWN);
	return keys;
}
