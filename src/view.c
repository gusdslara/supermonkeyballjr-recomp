#include "view.h"
#include "emu.h"
#include "input.h"
#include "hud.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------- tokens
static const SDL_FColor C_BTN = { 1.00f, 1.00f, 1.00f, 0.16f };
static const SDL_FColor C_BTN_EDGE = { 1.00f, 1.00f, 1.00f, 0.38f };
static const SDL_FColor C_BTN_ON = { 0.98f, 0.78f, 0.18f, 0.60f };  // banana
static const SDL_FColor C_ARC = { 1.00f, 1.00f, 1.00f, 0.13f };
static const SDL_FColor C_LABEL = { 1.00f, 1.00f, 1.00f, 0.85f };
static const SDL_FColor C_STICK = { 1.00f, 1.00f, 1.00f, 0.22f };
static const SDL_FColor C_KNOB = { 0.98f, 0.78f, 0.18f, 0.60f };
static const SDL_FColor C_TOAST_BG = { 0.05f, 0.05f, 0.07f, 0.78f };
static const SDL_FColor C_MASK = { 0, 0, 0, 1 };
#define TOAST_NS (1800ull * 1000000ull)
#define IDLE_FADE_NS (4000ull * 1000000ull)

#define PI_F 3.14159265f
#define DEG(d) ((d) * PI_F / 180.0f)

// Round screens:
//  - Menus / 2D screens (lots of text near the edges): never bent. The whole frame is the
//    largest 3:2 rectangle that fits the glass (IMG_RHO); the buttons fill the free caps
//    around it, inner edge on the picture border.
//  - In-stage 3D: the round lens fills the whole disc. Its core (where the ball is) is a
//    plain even zoom; only the outer band is compressed so the frame border meets the rim.
//    The HUD is redrawn natively on top, the buttons shrink to a thin band on the rim at the
//    same angles. Crown: lens strength (0 = whole flat frame in-stage too).
#define IMG_RHO 0.985f
#define BTN_GAP 0.022f       // picture-to-button gap (fraction of R)
#define ARC_OUT 0.985f
#define ARC_IN 0.915f        // in-stage rim band
#define ARC_HIT_RHO 0.64f    // in-stage: the rim touch band reaches inwards
// Lens: source radius (GBA px) of the frame corner, i.e. the plain inscribed fit.
#define SRC_CORNER 144.222f
#define LENS_C_MAX 74.0f     // GBA px per unit radius at full strength (bigger center)
#define LENS_RHO0_MAX 0.74f  // undistorted core radius at full strength
#define LENS_DEFAULT 0.55f
#define MESH_A 128
#define MESH_R 28

typedef enum { RECT_FIT = 0, RECT_WIDE, RECT_ZOOM, RECT_COUNT } RectMode;

// Ultrawide "smart stretch": undistorted core, only the sides stretch to fill the width.
#define WIDE_CORE 0.55f        // fraction of the GBA half-width kept 1:1
#define WIDE_MAX_ASPECT 2.40f  // beyond 21:9 the rest is pillarboxed
#define WIDE_EDGE_SLOPE 0.5f
#define WIDE_NX 96

typedef struct {
	float cx, cy, r;   // circle buttons (rectangular screens)
	float a0, a1;      // arc buttons (round screens), degrees, y-down
	bool arc;
	uint32_t bits;
	const char* label;
} Btn;

static struct {
	int w, h;
	bool round;
	float cx, cy, R;
	Btn btn[8];
	int nbtn;
	float zoom;  // rectangular screens, ZOOM mode: 0..1 extra crop
	RectMode rectMode;
	bool rectModeSet;  // user picked a mode (else auto by aspect)
	SDL_FRect area;    // rectangular screens: where the game content ends up this frame
	float lens;     // round screens, in-stage lens strength (0 = flat whole frame), user setting
	float lensCur;  // animated strength actually used (always 0 in menus)
	bool hud;       // in-stage HUD on screen (gameplay) -> zoomed scene + HUD plates
	bool overlay, mask, smooth;
	char toast[96];
	uint64_t toastNs;
	char debug[128];

	SDL_Texture* up;  // 3x nearest-upscaled frame, sampled linearly
	SDL_FRect pic;    // round screens: where the flat frame goes
	SDL_Vertex mesh[MESH_A * (MESH_R + 1)];
	int idx[MESH_A * MESH_R * 6];
	bool meshDirty;
	float meshLens;
	uint32_t lastHeld;
	uint64_t lastTouchNs;
	SDL_Vertex wide[(WIDE_NX + 1) * 2];
	int wideIdx[WIDE_NX * 6];
	float wideKey;
} V = { .zoom = 0.0f, .lens = LENS_DEFAULT, .lensCur = 0.0f, .overlay = true, .meshDirty = true };

// Native round HUD, built from the decoded stage sprites (see hud.h):
//  - HUD_RIBBON (timer bomb + progress fuse) is bent along the top rim, so the fuse curves
//    with the bezel and stays attached to the bomb
//  - HUD_SCORE / HUD_BANANA / HUD_FLOOR stay upright, transparent, tucked against the rim
//  - HUD_MID (GO!, GOAL!, results) is drawn at the whole-frame size, never zoomed off the glass
#define HUD_TOP_Y1 44
#define HUD_RIBBON_OUTER 0.905f  // stays inside the bezel buttons
#define HUD_SCALE_DIV 118.0f     // ribbon: screen px per GBA px = R / this
#define HUD_UPRIGHT_DIV 132.0f   // upright groups
#define HUD_UPRIGHT_RHO 0.90f
#define RIB_NX 60
#define RIB_NY 4

// Scene pixels the game leaves dirty under the timer bomb (a bitmap copy of the digits).
static const SDL_FRect SCENE_UNDER_BOMB = { 84, 0, 70, 12 };

static struct {
	const uint32_t* const* layers;
	SDL_Texture* tex[HUD_COUNT];
	SDL_Texture* up[HUD_COUNT];
	SDL_Vertex top[(RIB_NX + 1) * (RIB_NY + 1)];
	int idx[RIB_NX * RIB_NY * 6];
	SDL_FRect place[HUD_COUNT];
	float builtR;
	float dim;
} H = { .dim = 1.0f };

void view_set_hud_layers(const uint32_t* const* layers) {
	H.layers = layers;
	V.hud = layers != NULL;
}


void view_set_overlay(bool on) { V.overlay = on; }
bool view_overlay(void) { return V.overlay; }
void view_set_mask(bool on) { V.mask = on; }
void view_set_smooth(bool on) { V.smooth = on; }
bool view_smooth(void) { return V.smooth; }

static RectMode auto_rect_mode(void) {
	return (float) V.w / (float) V.h > 1.6f ? RECT_WIDE : RECT_FIT;
}

static RectMode rect_mode(void) {
	return V.rectModeSet ? V.rectMode : auto_rect_mode();
}

const char* view_cycle_fit(void) {
	if (V.round) {
		V.lens = V.lens >= 0.25f ? 0.0f : LENS_DEFAULT;
		return V.lens > 0 ? "Fase redonda" : "Fase plana";
	}
	V.rectMode = (RectMode) ((rect_mode() + 1) % RECT_COUNT);
	V.rectModeSet = true;
	if (V.rectMode == RECT_ZOOM && V.zoom < 0.2f) {
		V.zoom = 0.35f;
	}
	static const char* names[] = { "Tela: original 3:2", "Tela: widescreen", "Tela: zoom" };
	return names[V.rectMode];
}

void view_zoom(float d) {
	if (V.round) {
		V.lens = SDL_clamp(V.lens + d, 0.0f, 1.0f);
	} else {
		if (rect_mode() != RECT_ZOOM) {
			V.rectMode = RECT_ZOOM;
			V.rectModeSet = true;
			V.zoom = 0;
		}
		V.zoom = SDL_clamp(V.zoom + d, 0.0f, 1.0f);
	}
}

void view_toast(const char* msg) {
	SDL_strlcpy(V.toast, msg, sizeof(V.toast));
	V.toastNs = SDL_GetTicksNS();
	SDL_Log("%s", msg);
}

void view_set_debug(const char* t) {
	if (t) {
		SDL_strlcpy(V.debug, t, sizeof(V.debug));
	} else {
		V.debug[0] = 0;
	}
}

static void add_btn(float cx, float cy, float r, uint32_t bits, const char* label) {
	V.btn[V.nbtn++] = (Btn) { .cx = cx, .cy = cy, .r = r, .bits = bits, .label = label };
}

static void add_arc(float a0, float a1, uint32_t bits, const char* label) {
	V.btn[V.nbtn++] = (Btn) { .a0 = a0, .a1 = a1, .arc = true, .bits = bits, .label = label };
}

void view_layout(int w, int h, bool round) {
	V.w = w;
	V.h = h;
	V.round = round;
	V.cx = w * 0.5f;
	V.cy = h * 0.5f;
	V.R = (w < h ? w : h) * 0.5f;
	V.nbtn = 0;
	V.meshDirty = true;
	if (round) {
		// Degrees, 0 = right, 90 = bottom. All in the caps above / below the picture (its
		// corners sit at about +-34 / 180+-34 degrees): shoulders and MODE on top, A / B under
		// the thumbs with START / SELECT between them. The middle stays free for the swipe stick.
		add_arc(216, 250, KEY_L, "L");
		add_arc(256, 284, VIEW_MODE, "M");
		add_arc(290, 324, KEY_R, "R");
		add_arc(36, 68, KEY_A, "A");
		add_arc(72, 88, KEY_START, "ST");
		add_arc(92, 108, KEY_SELECT, "SE");
		add_arc(112, 144, KEY_B, "B");
	} else {
		float u = (float) (w < h ? w : h);
		add_btn(w - 0.17f * u, h * 0.58f, 0.10f * u, KEY_A, "A");
		add_btn(w - 0.38f * u, h * 0.74f, 0.085f * u, KEY_B, "B");
		// Corners belong to the HUD (SCORE, BANANA, FLOOR) and the top center to the timer bomb,
		// so shoulders sit on the side edges and MODE at the bottom center.
		add_btn(w - 0.14f * u, h - 0.10f * u, 0.06f * u, KEY_START, "ST");
		add_btn(0.12f * u, h - 0.30f * u, 0.06f * u, KEY_SELECT, "SE");
		add_btn(0.11f * u, 0.36f * u, 0.08f * u, KEY_L, "L");
		add_btn(w - 0.11f * u, 0.30f * u, 0.08f * u, KEY_R, "R");
		add_btn(w * 0.5f, h - 0.07f * u, 0.05f * u, VIEW_MODE, "M");
	}
}

// Half size of the whole frame drawn as the inscribed rectangle (zoom 1).
static void img_half(float* hw, float* hh) {
	float d = sqrtf((float) (EMU_W * EMU_W + EMU_H * EMU_H));
	float ri = V.R * IMG_RHO;
	*hw = ri * EMU_W / d;
	*hh = ri * EMU_H / d;
}

// Distance from the center to the picture border (zoom 1) along the angle (degrees, y-down).
static float border_dist(float deg) {
	float hw, hh;
	img_half(&hw, &hh);
	float a = DEG(deg);
	float c = fabsf(cosf(a)), s = fabsf(sinf(a));
	float dx = c > 1e-6f ? hw / c : 1e9f;
	float dy = s > 1e-6f ? hh / s : 1e9f;
	return dx < dy ? dx : dy;
}

// Buttons fill the whole cap while the whole frame is shown, a thin rim band in-stage.
static bool btn_full(void) {
	return V.lensCur < 0.01f;
}

static float btn_inner(float deg) {
	if (!btn_full()) {
		return ARC_IN * V.R;
	}
	return SDL_min(border_dist(deg) + BTN_GAP * V.R, ARC_OUT * V.R - 2.0f);
}

static bool angle_in(float deg, float a0, float a1) {
	float span = a1 - a0;
	float d = fmodf(deg - a0, 360.0f);
	if (d < 0) {
		d += 360.0f;
	}
	return d <= span;
}

uint32_t view_hit(float x, float y) {
	if (!V.overlay) {
		return 0;
	}
	float dx = x - V.cx, dy = y - V.cy;
	float dist = sqrtf(dx * dx + dy * dy);
	float deg = atan2f(dy, dx) * 180.0f / PI_F;
	// Whole frame: anywhere outside the picture. In-stage: the outer band of the disc.
	float inner = btn_full() ? border_dist(deg) + 0.5f * BTN_GAP * V.R : ARC_HIT_RHO * V.R;
	for (int i = 0; i < V.nbtn; ++i) {
		const Btn* b = &V.btn[i];
		if (b->arc) {
			if (dist >= inner && angle_in(deg, b->a0 - 2, b->a1 + 2)) {
				return b->bits;
			}
		} else {
			float bx = x - b->cx, by = y - b->cy;
			float r = b->r * 1.25f;  // generous touch slop
			if (bx * bx + by * by <= r * r) {
				return b->bits;
			}
		}
	}
	return 0;
}

// ---------------------------------------------------------------- primitives
#define SEG 40

static void fill_circle(SDL_Renderer* r, float cx, float cy, float rad, SDL_FColor c) {
	SDL_Vertex v[SEG + 1];
	int idx[SEG * 3];
	v[0] = (SDL_Vertex) { { cx, cy }, c, { 0, 0 } };
	for (int i = 0; i < SEG; ++i) {
		float a = i * 6.2831853f / SEG;
		v[i + 1] = (SDL_Vertex) { { cx + cosf(a) * rad, cy + sinf(a) * rad }, c, { 0, 0 } };
		idx[i * 3] = 0;
		idx[i * 3 + 1] = i + 1;
		idx[i * 3 + 2] = (i + 1) % SEG + 1;
	}
	SDL_RenderGeometry(r, NULL, v, SEG + 1, idx, SEG * 3);
}

// Ring segment between radii r0..r1 and angles a0..a1 (degrees). Full ring if a1 - a0 >= 360.
static void arc_band(SDL_Renderer* r, float cx, float cy, float r0, float r1, float a0, float a1, SDL_FColor c) {
	enum { N = 40 };
	SDL_Vertex v[(N + 1) * 2];
	int idx[N * 6];
	for (int i = 0; i <= N; ++i) {
		float a = DEG(a0 + (a1 - a0) * i / N);
		float ca = cosf(a), sa = sinf(a);
		v[i * 2] = (SDL_Vertex) { { cx + ca * r0, cy + sa * r0 }, c, { 0, 0 } };
		v[i * 2 + 1] = (SDL_Vertex) { { cx + ca * r1, cy + sa * r1 }, c, { 0, 0 } };
	}
	for (int i = 0; i < N; ++i) {
		int* q = &idx[i * 6];
		q[0] = i * 2; q[1] = i * 2 + 1; q[2] = i * 2 + 2;
		q[3] = i * 2 + 2; q[4] = i * 2 + 1; q[5] = i * 2 + 3;
	}
	SDL_RenderGeometry(r, NULL, v, (N + 1) * 2, idx, N * 6);
}

static void ring(SDL_Renderer* r, float cx, float cy, float r0, float r1, SDL_FColor c) {
	arc_band(r, cx, cy, r0, r1, 0, 360, c);
}

// Round-screen button: from its inner edge (picture border or rim band) out to the rim.
static void btn_band(SDL_Renderer* r, float a0, float a1, SDL_FColor c) {
	enum { N = 32 };
	SDL_Vertex v[(N + 1) * 2];
	int idx[N * 6];
	float ro = ARC_OUT * V.R;
	for (int i = 0; i <= N; ++i) {
		float deg = a0 + (a1 - a0) * i / N;
		float a = DEG(deg);
		float ca = cosf(a), sa = sinf(a), ri = btn_inner(deg);
		v[i * 2] = (SDL_Vertex) { { V.cx + ca * ri, V.cy + sa * ri }, c, { 0, 0 } };
		v[i * 2 + 1] = (SDL_Vertex) { { V.cx + ca * ro, V.cy + sa * ro }, c, { 0, 0 } };
	}
	for (int i = 0; i < N; ++i) {
		int* q = &idx[i * 6];
		q[0] = i * 2; q[1] = i * 2 + 1; q[2] = i * 2 + 2;
		q[3] = i * 2 + 2; q[4] = i * 2 + 1; q[5] = i * 2 + 3;
	}
	SDL_RenderGeometry(r, NULL, v, (N + 1) * 2, idx, N * 6);
}

static void text_centered(SDL_Renderer* r, float cx, float cy, float px, const char* s, SDL_FColor c) {
	// Whole-number scale and pixel-aligned origin: the 8x8 font stays sharp (a fractional
	// scale smears it into uneven, blurry strokes).
	float k = floorf(px / (float) SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE);
	if (k < 1) {
		k = 1;
	}
	float tw = (float) strlen(s) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
	SDL_SetRenderScale(r, k, k);
	SDL_SetRenderDrawColorFloat(r, c.r, c.g, c.b, c.a);
	SDL_RenderDebugText(r, roundf(cx / k - tw * 0.5f), roundf(cy / k - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * 0.5f), s);
	SDL_SetRenderScale(r, 1, 1);
}

// ---------------------------------------------------------------- round lens (in-stage)
//
// Maps the whole 240x160 frame onto the disc without cropping anything: a core of radius
// rho0 is a plain uniform zoom (no distortion where the ball is), the outer band is
// compressed radially so the frame border lands on the rim. At strength 0 this is exactly
// the flat inscribed rectangle the menus use, so the switch between them is seamless.

static float lens_source_radius(float rho, float L, float c, float rho0) {
	if (rho0 >= 0.999f) {
		return c * rho;
	}
	// Edge curve: cubic Hermite from the linear core (slope matched) to the frame border at
	// the rim (zero end slope). Monotonic needs m <= 3; when a short ray (top/bottom) can't
	// satisfy that, its undistorted core shrinks just enough.
	float r0 = c * rho0;
	float m = L > r0 ? c * (1.0f - rho0) / (L - r0) : 1e9f;
	if (m > 3.0f) {
		rho0 = SDL_max(0.0f, (3.0f * L - c) / (2.0f * c));
		r0 = c * rho0;
		m = c * (1.0f - rho0) / (L - r0);
	}
	if (rho <= rho0) {
		return c * rho;
	}
	float t = (rho - rho0) / (1.0f - rho0);
	float h = m * t + (3.0f - 2.0f * m) * t * t + (m - 2.0f) * t * t * t;
	return r0 + (L - r0) * h;
}

static void build_mesh(void) {
	float c = SRC_CORNER + (LENS_C_MAX - SRC_CORNER) * V.lensCur;
	float rho0 = 1.0f + (LENS_RHO0_MAX - 1.0f) * V.lensCur;
	float Ri = V.R * IMG_RHO;
	V.meshLens = V.lensCur;
	const SDL_FColor white = { 1, 1, 1, 1 };
	for (int a = 0; a < MESH_A; ++a) {
		float th = a * 2.0f * PI_F / MESH_A;
		float ct = cosf(th), st = sinf(th);
		float lx = fabsf(ct) > 1e-5f ? (EMU_W * 0.5f) / fabsf(ct) : 1e9f;
		float ly = fabsf(st) > 1e-5f ? (EMU_H * 0.5f) / fabsf(st) : 1e9f;
		float L = lx < ly ? lx : ly;  // distance to the frame border along this ray
		float rhoEnd = lens_source_radius(1.0f, L, c, rho0) <= L + 0.01f ? 1.0f : L / c;
		if (rhoEnd > 1.0f) {
			rhoEnd = 1.0f;
		}
		for (int i = 0; i <= MESH_R; ++i) {
			float rho = rhoEnd * i / MESH_R;
			float sr = lens_source_radius(rho, L, c, rho0);
			if (sr > L) {
				sr = L;
			}
			SDL_Vertex* v = &V.mesh[a * (MESH_R + 1) + i];
			v->position.x = V.cx + ct * rho * Ri;
			v->position.y = V.cy + st * rho * Ri;
			v->color = white;
			v->tex_coord.x = (EMU_W * 0.5f + ct * sr) / EMU_W;
			v->tex_coord.y = (EMU_H * 0.5f + st * sr) / EMU_H;
		}
	}
	int k = 0;
	for (int a = 0; a < MESH_A; ++a) {
		int a1 = (a + 1) % MESH_A;
		for (int i = 0; i < MESH_R; ++i) {
			int p00 = a * (MESH_R + 1) + i, p01 = p00 + 1;
			int p10 = a1 * (MESH_R + 1) + i, p11 = p10 + 1;
			V.idx[k++] = p00; V.idx[k++] = p10; V.idx[k++] = p01;
			V.idx[k++] = p01; V.idx[k++] = p10; V.idx[k++] = p11;
		}
	}
	V.meshDirty = false;
}

// Ribbon mesh for GBA rows [y0, y1): x runs along the top rim, y runs inward.
static void build_ribbon(SDL_Vertex* v, float y0, float y1) {
	float s = V.R / HUD_SCALE_DIV;
	float k = s / (HUD_RIBBON_OUTER * V.R);  // radians per GBA px along the outer edge
	const SDL_FColor white = { 1, 1, 1, 1 };
	for (int j = 0; j <= RIB_NY; ++j) {
		float y = y0 + (y1 - y0) * j / RIB_NY;
		float rho = HUD_RIBBON_OUTER - (y - y0) * s / V.R;
		for (int i = 0; i <= RIB_NX; ++i) {
			float x = (float) EMU_W * i / RIB_NX;
			float th = -PI_F * 0.5f + (x - EMU_W * 0.5f) * k;
			SDL_Vertex* p = &v[j * (RIB_NX + 1) + i];
			p->position.x = V.cx + cosf(th) * rho * V.R;
			p->position.y = V.cy + sinf(th) * rho * V.R;
			p->color = white;
			p->tex_coord.x = x / EMU_W;
			p->tex_coord.y = y / EMU_H;
		}
	}
}

// Pushes an upright w x h box outward along the direction of its original position until
// its far corners touch radius `rho`.
static SDL_FRect place_upright(const float* src, float dirDeg, float scale, float rho) {
	float w = src[2] * scale, h = src[3] * scale;
	float dx = cosf(DEG(dirDeg)), dy = sinf(DEG(dirDeg));
	float lim = rho * V.R, lo = 0, hi = V.R;
	for (int it = 0; it < 24; ++it) {
		float d = (lo + hi) * 0.5f;
		float x0 = dx * d - w * 0.5f, x1 = x0 + w, y0 = dy * d - h * 0.5f, y1 = y0 + h;
		float fx = SDL_max(fabsf(x0), fabsf(x1)), fy = SDL_max(fabsf(y0), fabsf(y1));
		if (fx * fx + fy * fy <= lim * lim) {
			lo = d;
		} else {
			hi = d;
		}
	}
	return (SDL_FRect) { V.cx + dx * lo - w * 0.5f, V.cy + dy * lo - h * 0.5f, w, h };
}

static void ensure_hud_resources(SDL_Renderer* r) {
	if (!H.tex[0]) {
		for (int g = 0; g < HUD_COUNT; ++g) {
			H.tex[g] = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, EMU_W, EMU_H);
			SDL_SetTextureScaleMode(H.tex[g], SDL_SCALEMODE_NEAREST);
			SDL_SetTextureBlendMode(H.tex[g], SDL_BLENDMODE_BLEND);
		}
		// Warped groups get a crisp 3x copy that is then sampled bilinearly.
		int warped[] = { HUD_MID, HUD_RIBBON };
		for (int i = 0; i < 2; ++i) {
			int g = warped[i];
			H.up[g] = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, EMU_W * 3, EMU_H * 3);
			SDL_SetTextureScaleMode(H.up[g], SDL_SCALEMODE_LINEAR);
			SDL_SetTextureBlendMode(H.up[g], SDL_BLENDMODE_BLEND);
		}
		int k = 0;
		for (int j = 0; j < RIB_NY; ++j) {
			for (int i = 0; i < RIB_NX; ++i) {
				int p = j * (RIB_NX + 1) + i;
				H.idx[k++] = p; H.idx[k++] = p + 1; H.idx[k++] = p + RIB_NX + 1;
				H.idx[k++] = p + 1; H.idx[k++] = p + RIB_NX + 2; H.idx[k++] = p + RIB_NX + 1;
			}
		}
	}
	if (H.builtR != V.R) {
		build_ribbon(H.top, 0, HUD_TOP_Y1);
		float s = V.R / HUD_UPRIGHT_DIV;
		for (int g = HUD_SCORE; g < HUD_COUNT; ++g) {
			H.place[g] = place_upright(HUD_GROUP_RECT[g], HUD_GROUP_DIR[g], s, HUD_UPRIGHT_RHO);
		}
		H.builtR = V.R;
	}
}

static void upscale_alpha(SDL_Renderer* r, SDL_Texture* src, SDL_Texture* dst) {
	SDL_SetRenderTarget(r, dst);
	SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
	SDL_RenderClear(r);
	SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
	SDL_RenderTexture(r, src, NULL, NULL);
	SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
	SDL_SetRenderTarget(r, NULL);
}

static float hud_busy_dim(void);

static void render_hud(SDL_Renderer* r) {
	ensure_hud_resources(r);
	for (int g = 0; g < HUD_COUNT; ++g) {
		SDL_UpdateTexture(H.tex[g], NULL, H.layers[g], EMU_W * 4);
	}
	upscale_alpha(r, H.tex[HUD_MID], H.up[HUD_MID]);
	upscale_alpha(r, H.tex[HUD_RIBBON], H.up[HUD_RIBBON]);
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

	// GO!, GOAL! and the results span almost the whole frame width: always drawn at the
	// whole-frame size so the in-stage zoom never pushes them off the glass.
	float hw, hh;
	img_half(&hw, &hh);
	SDL_FRect mid = { roundf(V.cx - hw), roundf(V.cy - hh), roundf(2.0f * hw), roundf(2.0f * hh) };
	SDL_RenderTexture(r, H.up[HUD_MID], NULL, &mid);
	SDL_RenderGeometry(r, H.up[HUD_RIBBON], H.top, (RIB_NX + 1) * (RIB_NY + 1), H.idx, RIB_NX * RIB_NY * 6);
	float dim = hud_busy_dim();
	for (int g = HUD_SCORE; g < HUD_COUNT; ++g) {
		const float* s = HUD_GROUP_RECT[g];
		SDL_FRect src = { s[0], s[1], s[2], s[3] };
		SDL_SetTextureAlphaModFloat(H.tex[g], dim);
		SDL_RenderTexture(r, H.tex[g], &src, &H.place[g]);
	}
}

// Crisp pixels: nearest 3x upscale (with the dirty area under the lifted bomb patched), then
// every warp samples it bilinearly. Returns the texture to sample.
static SDL_Texture* prepare_scene(SDL_Renderer* r, SDL_Texture* game) {
	if (!V.up) {
		V.up = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, EMU_W * 3, EMU_H * 3);
	}
	SDL_Texture* src = game;
	if (V.up && !V.smooth) {
		SDL_SetTextureScaleMode(game, SDL_SCALEMODE_NEAREST);
		SDL_SetRenderTarget(r, V.up);
		SDL_RenderTexture(r, game, NULL, NULL);
		if (V.hud) {
			const SDL_FRect* u = &SCENE_UNDER_BOMB;
			SDL_FRect row = { u->x, u->y + u->h, u->w, 1 };
			SDL_FRect dst = { u->x * 3, u->y * 3, u->w * 3, u->h * 3 };
			SDL_RenderTexture(r, game, &row, &dst);
		}
		SDL_SetRenderTarget(r, NULL);
		src = V.up;
	}
	SDL_SetTextureScaleMode(src, SDL_SCALEMODE_LINEAR);
	return src;
}

static float hud_busy_dim(void) {
	// Big center messages (GOAL! results) take priority: the side groups step aside
	// completely meanwhile (dimmed plates still collided with the results text).
	const uint32_t* mid = H.layers[HUD_MID];
	int busy = 0;
	for (int i = 40 * EMU_W; i < 100 * EMU_W; i += 3) {
		busy += (mid[i] >> 24) != 0;
	}
	float a = busy > 150 ? 0.0f : 1.0f;
	H.dim += SDL_clamp(a - H.dim, -0.08f, 0.08f);
	return H.dim;
}

static void draw_mapped(SDL_Renderer* r, SDL_Texture* tex, RectMode mode, const SDL_FRect* dst);

// Rectangular screens: the HUD hugs the corners of the visible game area (so it stays fully
// readable in widescreen and zoom), timer bomb + fuse stay centered on top.
static void render_hud_rect(SDL_Renderer* r, RectMode mode, const SDL_FRect* dst, float s) {
	ensure_hud_resources(r);
	for (int g = 0; g < HUD_COUNT; ++g) {
		SDL_UpdateTexture(H.tex[g], NULL, H.layers[g], EMU_W * 4);
	}
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	upscale_alpha(r, H.tex[HUD_MID], H.up[HUD_MID]);
	draw_mapped(r, mode == RECT_WIDE ? H.up[HUD_MID] : H.tex[HUD_MID], mode, dst);

	const SDL_FRect* a = &V.area;
	SDL_FRect rsrc = { 0, 0, EMU_W, HUD_TOP_Y1 };
	SDL_FRect rdst = { V.cx - EMU_W * 0.5f * s, a->y, EMU_W * s, HUD_TOP_Y1 * s };
	SDL_RenderTexture(r, H.tex[HUD_RIBBON], &rsrc, &rdst);

	float dim = hud_busy_dim();
	for (int g = HUD_SCORE; g < HUD_COUNT; ++g) {
		const float* q = HUD_GROUP_RECT[g];
		SDL_FRect src = { q[0], q[1], q[2], q[3] };
		bool left = q[0] + q[2] * 0.5f < EMU_W * 0.5f;
		bool bottom = q[1] > EMU_H * 0.5f;
		float x = left ? a->x + q[0] * s : a->x + a->w - (EMU_W - q[0]) * s;
		float y = bottom ? a->y + a->h - (EMU_H - q[1]) * s : a->y + q[1] * s;
		SDL_FRect d = { x, y, q[2] * s, q[3] * s };
		SDL_SetTextureAlphaModFloat(H.tex[g], dim);
		SDL_RenderTexture(r, H.tex[g], &src, &d);
	}
}

static void render_round(SDL_Renderer* r, SDL_Texture* game) {
	// Menus show the whole frame; the 3D stage gets the even zoom.
	float target = V.hud ? V.lens : 0.0f;
	V.lensCur += SDL_clamp(target - V.lensCur, -0.04f, 0.04f);
	float hw, hh;
	img_half(&hw, &hh);
	V.pic = (SDL_FRect) { roundf(V.cx - hw), roundf(V.cy - hh), roundf(2.0f * hw), roundf(2.0f * hh) };
	SDL_Texture* src = prepare_scene(r, game);
	if (V.lensCur < 0.002f) {
		SDL_RenderTexture(r, src, NULL, &V.pic);  // flat whole frame (menus)
	} else {
		if (V.meshDirty || fabsf(V.lensCur - V.meshLens) > 0.001f) {
			build_mesh();
		}
		SDL_RenderGeometry(r, src, V.mesh, MESH_A * (MESH_R + 1), V.idx, MESH_A * MESH_R * 6);
	}

	if (V.hud && H.layers) {
		render_hud(r);
	}
}

// ---------------------------------------------------------------- rectangular screens

static SDL_FRect game_rect(float zoom) {
	float sx = (float) V.w / EMU_W, sy = (float) V.h / EMU_H;
	float sFit = sx < sy ? sx : sy;
	float s = sFit * (1.0f + 0.5f * zoom);
	float gw = EMU_W * s, gh = EMU_H * s;
	return (SDL_FRect) { V.cx - gw * 0.5f, V.cy - gh * 0.5f, gw, gh };
}

// Screen offset d (px from center) -> GBA offset u for the smart-stretch.
static float wide_source(float d, float s, float halfW) {
	float u0 = EMU_W * 0.5f * WIDE_CORE;
	float d0 = u0 * s;
	if (d <= d0 || halfW <= EMU_W * 0.5f * s) {
		return d / s;
	}
	float span = EMU_W * 0.5f - u0;
	float t = SDL_min(1.0f, (d - d0) / (halfW - d0));
	float m0 = (halfW - d0) / s / span;  // matches the 1:1 core slope
	float m1 = WIDE_EDGE_SLOPE;
	float t2 = t * t, t3 = t2 * t;
	float h = (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) + (t3 - t2) * m1;
	return u0 + span * h;
}

static void build_wide(SDL_FRect area, float s) {
	const SDL_FColor white = { 1, 1, 1, 1 };
	float halfW = area.w * 0.5f;
	for (int i = 0; i <= WIDE_NX; ++i) {
		float x = area.x + area.w * i / WIDE_NX;
		float d = x - V.cx;
		float u = wide_source(fabsf(d), s, halfW);
		float tx = (EMU_W * 0.5f + (d < 0 ? -u : u)) / EMU_W;
		V.wide[i * 2] = (SDL_Vertex) { { x, area.y }, white, { tx, 0 } };
		V.wide[i * 2 + 1] = (SDL_Vertex) { { x, area.y + area.h }, white, { tx, 1 } };
		if (i < WIDE_NX) {
			int* q = &V.wideIdx[i * 6];
			q[0] = i * 2; q[1] = i * 2 + 2; q[2] = i * 2 + 1;
			q[3] = i * 2 + 1; q[4] = i * 2 + 2; q[5] = i * 2 + 3;
		}
	}
}

// Draws `tex` (a full 240x160 frame-space texture) with the current rectangular mapping.
static void draw_mapped(SDL_Renderer* r, SDL_Texture* tex, RectMode mode, const SDL_FRect* dst) {
	if (mode == RECT_WIDE) {
		SDL_RenderGeometry(r, tex, V.wide, (WIDE_NX + 1) * 2, V.wideIdx, WIDE_NX * 6);
	} else {
		SDL_RenderTexture(r, tex, NULL, dst);
	}
}

static void render_hud_rect(SDL_Renderer* r, RectMode mode, const SDL_FRect* dst, float s);

static void render_rect(SDL_Renderer* r, SDL_Texture* game) {
	RectMode mode = rect_mode();
	SDL_FRect dst = game_rect(mode == RECT_ZOOM ? V.zoom : 0.0f);
	float s = dst.w / EMU_W;
	if (mode == RECT_WIDE) {
		s = (float) V.h / EMU_H;
		float w = SDL_min((float) V.w, V.h * WIDE_MAX_ASPECT);
		V.area = (SDL_FRect) { V.cx - w * 0.5f, 0, w, (float) V.h };
		float key = V.area.w * 10000.0f + V.area.h;
		if (key != V.wideKey) {
			build_wide(V.area, s);
			V.wideKey = key;
		}
	} else {
		// Visible part of the game rect (zoom may overflow the screen).
		float x0 = SDL_max(0.0f, dst.x), y0 = SDL_max(0.0f, dst.y);
		float x1 = SDL_min((float) V.w, dst.x + dst.w), y1 = SDL_min((float) V.h, dst.y + dst.h);
		V.area = (SDL_FRect) { x0, y0, x1 - x0, y1 - y0 };
	}
	SDL_Texture* src = prepare_scene(r, game);
	if (V.smooth || mode == RECT_WIDE) {
		SDL_SetTextureScaleMode(src, SDL_SCALEMODE_LINEAR);
	} else {
		SDL_SetTextureScaleMode(src, SDL_SCALEMODE_NEAREST);
	}
	draw_mapped(r, src, mode, &dst);
	if (V.hud && H.layers) {
		render_hud_rect(r, mode, &dst, s);
	}
}

static SDL_FColor faded(SDL_FColor c, float k) {
	c.a *= k;
	return c;
}

static void render_overlay(SDL_Renderer* r) {
	uint32_t held = input_touch_held();
	uint64_t now = SDL_GetTicksNS();
	const TouchStick* st = input_stick();
	if (held || st->active || V.lastTouchNs == 0) {
		V.lastTouchNs = now;
	}
	// Controls dim after a few seconds without touches so the picture stays clean.
	float k = now - V.lastTouchNs > IDLE_FADE_NS ? 0.45f : 1.0f;

	for (int i = 0; i < V.nbtn; ++i) {
		const Btn* b = &V.btn[i];
		bool on = (held & b->bits) != 0;
		if (b->arc) {
			btn_band(r, b->a0, b->a1, on ? C_BTN_ON : faded(C_ARC, k));
			float midDeg = (b->a0 + b->a1) * 0.5f;
			float mid = DEG(midDeg);
			float ri = btn_inner(midDeg), ro = ARC_OUT * V.R;
			float lr = (ri + ro) * 0.5f;
			// 8 px font at 1x-3x, never wider than the button.
			float wide = (b->a1 - b->a0) * PI_F / 180.0f * lr * 0.8f / (float) strlen(b->label);
			float px = SDL_clamp(SDL_min((ro - ri) * 0.55f, wide), 8.0f, 24.0f);
			text_centered(r, V.cx + cosf(mid) * lr, V.cy + sinf(mid) * lr, px, b->label,
			              faded(C_LABEL, on ? 1.0f : 0.75f * k));
		} else {
			fill_circle(r, b->cx, b->cy, b->r, on ? C_BTN_ON : faded(C_BTN, k));
			ring(r, b->cx, b->cy, b->r - SDL_max(1.5f, b->r * 0.08f), b->r, faded(C_BTN_EDGE, k));
			text_centered(r, b->cx, b->cy, b->r * 0.75f, b->label, faded(C_LABEL, k));
		}
	}
	if (st->active) {
		ring(r, st->ax, st->ay, st->radius - 2, st->radius, C_STICK);
		fill_circle(r, st->x, st->y, st->radius * 0.32f, C_KNOB);
	}
}

// Toast: 16 px glyphs (8 px on tiny screens), split over two lines when one does not fit.
static void render_toast(SDL_Renderer* r, float y, float maxW, float alpha) {
	const float g = V.R >= 150.0f ? 16.0f : 8.0f;
	const char* msg = V.toast;
	size_t n = strlen(msg);
	char line[2][96];
	int lines = 1;
	SDL_strlcpy(line[0], msg, sizeof(line[0]));
	line[1][0] = 0;
	if (n * g > maxW) {
		size_t best = 0;
		for (size_t c = 1; c < n; ++c) {
			if (msg[c] == ' ' && (best == 0 || SDL_abs((int) c - (int) n / 2) < SDL_abs((int) best - (int) n / 2))) {
				best = c;
			}
		}
		if (best) {
			SDL_strlcpy(line[0], msg, best + 1);
			SDL_strlcpy(line[1], msg + best + 1, sizeof(line[1]));
			lines = 2;
		}
	}
	size_t widest = SDL_max(strlen(line[0]), strlen(line[1]));
	float tw = (float) widest * g, step = g * 1.35f;
	float th = step * (float) lines + g * 0.5f;
	SDL_FRect bg = { roundf(V.cx - tw * 0.5f - g * 0.6f), roundf(y - th * 0.5f), roundf(tw + g * 1.2f), roundf(th) };
	SDL_SetRenderDrawColorFloat(r, C_TOAST_BG.r, C_TOAST_BG.g, C_TOAST_BG.b, C_TOAST_BG.a * alpha);
	SDL_RenderFillRect(r, &bg);
	for (int l = 0; l < lines; ++l) {
		float ly = y + ((float) l - (lines - 1) * 0.5f) * step;
		text_centered(r, V.cx, ly, g, line[l], faded(C_LABEL, alpha));
	}
}

void view_render(SDL_Renderer* r, SDL_Texture* game) {
	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
	SDL_RenderClear(r);

	if (V.round) {
		render_round(r, game);
	} else {
		render_rect(r, game);
	}

	if (V.overlay && input_touch_mode()) {
		render_overlay(r);
	}

	if (V.round && V.mask) {
		float far = sqrtf((float) (V.w * V.w + V.h * V.h));
		ring(r, V.cx, V.cy, V.R, far, C_MASK);
	}

	uint64_t now = SDL_GetTicksNS();
	if (V.toast[0] && now - V.toastNs < TOAST_NS) {
		float t = (float) (now - V.toastNs) / (float) TOAST_NS;
		float alpha = t > 0.8f ? (1.0f - t) / 0.2f : 1.0f;
		float y = V.h * 0.18f, maxW = V.w * 0.9f;
		if (V.round) {
			// In the top cap above the picture, never over the game's own text.
			float hw, hh;
			img_half(&hw, &hh);
			y = V.cy - (hh + V.R) * 0.5f;
			float dy = y - V.cy;
			maxW = 2.0f * sqrtf(SDL_max(0.0f, V.R * V.R - dy * dy)) * 0.86f;
		}
		render_toast(r, y, maxW, alpha);
	}

	if (V.debug[0]) {
		float px = SDL_max(8.0f, V.R * 0.05f);
		float y = V.round ? V.cy + V.R * 0.55f : V.h - px * 1.5f;
		text_centered(r, V.cx, y, px, V.debug, C_LABEL);
	}
}
