// Super Monkey Ball Jr. native shell — SDL3 app callbacks (desktop, Android, Wear OS, Web).
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "emu.h"
#include "recomp/recomp.h"
#include "input.h"
#include "view.h"
#include "hud.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define APP_ORG "wearosports"
#define APP_NAME "smbjr"
#define AUDIO_MAX_QUEUE_FRAMES (EMU_AUDIO_RATE / 10)  // 100 ms

static struct {
	SDL_Window* win;
	SDL_Renderer* ren;
	SDL_Texture* tex;
	SDL_AudioStream* audio;
	bool running;  // ROM loaded
	bool paused;
	bool fast;
	bool showDebug;
	int testFrames;  // --test N: scripted autoplay + screenshots, then quit
	bool interp;  // --interp: run the game on the interpreter instead of the recompiled code
	const char* missesPath;  // --misses FILE: addresses that fell back to the interpreter, written at exit
	SDL_IOStream* hashLog;  // --hashlog FILE: per-frame state hash (native vs. interpreter check)
	int frameNo;
	char err[512];
	char note[512];  // last ROM pick error (kept while the loader keeps polling)
	char base[1024];
	char romPath[1024];
	char savePath[1024];
	char statePath[1024];
	uint64_t lastNs;
	double accNs;
	double frameNs;
	int fpsFrames;
	uint64_t fpsStartNs;
	double fpsValue;
} A;

static void relayout(void) {
	int w, h;
	SDL_GetRenderOutputSize(A.ren, &w, &h);
	bool round = SDL_GetHintBoolean("SMBJR_ROUND", false);
	view_layout(w, h, round);
	input_set_screen(w, h);
}

static bool file_exists(const char* p) {
	SDL_PathInfo info;
	return p[0] && SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE;
}

static void usage(void) {
	SDL_Log("uso: smbjr [rom.gba] [--round] [--size WxH] [--touch] [--smooth]");
	SDL_Log("teclas: setas/WASD  Z=A X=B Enter=Start Backspace=Select Q=L E=R");
	SDL_Log("        F1 modo controle  F2 calibrar  F3 fit/fill  F4 overlay  F5 salvar estado");
	SDL_Log("        F7 carregar estado  F6 filtro  F8 PWM  F9 debug  Tab turbo  P pausa  F11 tela cheia");
	SDL_Log("        Ctrl+O escolher/trocar ROM (ou arraste o .gba para a janela)");
}

// GBA header check: NULL if the file is Super Monkey Ball Jr. (any region), else the reason.
static const char* rom_check(const char* path) {
	SDL_IOStream* io = SDL_IOFromFile(path, "rb");
	if (!io) {
		return "Nao foi possivel abrir o arquivo";
	}
	Uint8 h[0xC0];
	size_t n = SDL_ReadIO(io, h, sizeof(h));
	SDL_CloseIO(io);
	if (n < sizeof(h) || h[0xB2] != 0x96) {
		return "Arquivo nao e uma ROM de GBA (.zip? descompacte antes)";
	}
	if (SDL_memcmp(h + 0xA0, "MONKEYBALLJR", 12) != 0) {
		return "ROM de outro jogo: escolha Super Monkey Ball Jr.";
	}
	return NULL;
}

// Loads the ROM if it is there. Also polled while waiting (web upload, adb push, file copy).
static bool try_load_rom(void) {
	if (!file_exists(A.romPath)) {
		SDL_snprintf(A.err, sizeof(A.err), "ROM nao encontrada");
		return false;
	}
	const char* bad = rom_check(A.romPath);
	if (bad) {
		SDL_strlcpy(A.err, bad, sizeof(A.err));
		return false;
	}
	if (!emu_init(A.romPath, A.savePath, A.err, sizeof(A.err))) {
		return false;
	}
	A.running = true;
	if (A.interp) {
		recomp_set_enabled(false);
	}
	A.frameNs = 1e9 / emu_fps();
	emu_set_hud_split(true);
	A.lastNs = A.fpsStartNs = SDL_GetTicksNS();
	A.accNs = 0;
	SDL_Log("ROM: %s  (%.4f fps)  save: %s", A.romPath, emu_fps(), A.savePath);
	return true;
}

#if !defined(__EMSCRIPTEN__) && !defined(SDL_PLATFORM_ANDROID)
// Desktop ROM picking: file dialog (click / Ctrl+O) or drag & drop. The chosen file is
// validated and copied to the pref dir as rom.gba, so the next launch finds it by itself.
static SDL_Mutex* s_pickLock;
static char s_picked[1024];
static SDL_AtomicInt s_dialogOpen;

static void SDLCALL on_rom_dialog(void* ud, const char* const* files, int filter) {
	SDL_LockMutex(s_pickLock);
	if (files && files[0]) {
		SDL_strlcpy(s_picked, files[0], sizeof(s_picked));
	} else if (!files) {
		SDL_Log("dialogo: %s", SDL_GetError());
	}
	SDL_UnlockMutex(s_pickLock);
	SDL_SetAtomicInt(&s_dialogOpen, 0);
}

static void open_rom_dialog(void) {
	if (!SDL_CompareAndSwapAtomicInt(&s_dialogOpen, 0, 1)) {
		return;
	}
	static const SDL_DialogFileFilter filters[] = { { "ROM de GBA (*.gba)", "gba" }, { "Todos os arquivos", "*" } };
	SDL_ShowOpenFileDialog(on_rom_dialog, NULL, A.win, filters, 2, NULL, false);
}

static void import_rom(const char* path) {
	const char* bad = rom_check(path);
	if (bad) {
		SDL_strlcpy(A.note, bad, sizeof(A.note));
		view_toast(bad);
		SDL_Log("%s: %s", bad, path);
		return;
	}
	char dst[1024];
	SDL_snprintf(dst, sizeof(dst), "%srom.gba", A.base);
	// The core keeps the ROM file open: release it before overwriting (Windows locks it).
	if (A.running) {
		emu_shutdown();
		A.running = false;
	}
	if (SDL_strcmp(path, dst) != 0 && !SDL_CopyFile(path, dst)) {
		SDL_snprintf(A.note, sizeof(A.note), "Falha ao copiar ROM: %s", SDL_GetError());
		view_toast("Falha ao copiar ROM");
		try_load_rom();  // keep playing the previous one, if any
		return;
	}
	SDL_strlcpy(A.romPath, dst, sizeof(A.romPath));
	A.note[0] = 0;
	if (try_load_rom()) {
		view_toast("ROM carregada");
	} else {
		SDL_strlcpy(A.note, A.err, sizeof(A.note));
	}
}

static void poll_picked_rom(void) {
	char path[1024];
	SDL_LockMutex(s_pickLock);
	SDL_strlcpy(path, s_picked, sizeof(path));
	s_picked[0] = 0;
	SDL_UnlockMutex(s_pickLock);
	if (path[0]) {
		import_rom(path);
	}
}
#define HAS_ROM_PICKER 1
#endif

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
	SDL_SetAppMetadata("Super Recomp Ball Jr.", "0.1.0", "com.wearosports.smbjr");
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");  // mouse simulates a finger on desktop
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");

	int winW = 720, winH = 480;
	const char* romArg = NULL;
	bool round = false;
	bool forceTouch = false;
#if defined(SDL_PLATFORM_ANDROID)
	// Wear OS builds pass the round flag through the manifest metadata -> hint.
	round = SDL_GetHintBoolean("SMBJR_ROUND", false);
#endif
	for (int i = 1; i < argc; ++i) {
		if (!SDL_strcmp(argv[i], "--round")) {
			round = true;
			winW = winH = 454;
		} else if (!SDL_strcmp(argv[i], "--size") && i + 1 < argc) {
			SDL_sscanf(argv[++i], "%dx%d", &winW, &winH);
		} else if (!SDL_strcmp(argv[i], "--test") && i + 1 < argc) {
			A.testFrames = SDL_atoi(argv[++i]);
		} else if (!SDL_strcmp(argv[i], "--interp")) {
			A.interp = true;
		} else if (!SDL_strcmp(argv[i], "--misses") && i + 1 < argc) {
			A.missesPath = argv[++i];
		} else if (!SDL_strcmp(argv[i], "--hashlog") && i + 1 < argc) {
			A.hashLog = SDL_IOFromFile(argv[++i], "w");
		} else if (!SDL_strcmp(argv[i], "--touch")) {
			forceTouch = true;
		} else if (!SDL_strcmp(argv[i], "--smooth")) {
			view_set_smooth(true);
		} else if (!SDL_strcmp(argv[i], "--help")) {
			usage();
			return SDL_APP_SUCCESS;
		} else if (argv[i][0] != '-') {
			romArg = argv[i];
		}
	}
	SDL_SetHint("SMBJR_ROUND", round ? "1" : "0");

#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
	// Phones play in landscape (either side up); watches keep their fixed orientation.
	if (!round) {
		SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	}
#endif
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
		SDL_Log("SDL_Init: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	SDL_WindowFlags wf = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(SDL_PLATFORM_ANDROID)
	wf |= SDL_WINDOW_FULLSCREEN;
#endif

	if (!SDL_CreateWindowAndRenderer("Super Recomp Ball Jr.", winW, winH, wf, &A.win, &A.ren)) {
		SDL_Log("janela: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}
	SDL_SetRenderVSync(A.ren, A.testFrames ? 0 : 1);
	SDL_Log("Renderer: %s", SDL_GetRendererName(A.ren));
	A.tex = SDL_CreateTexture(A.ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, EMU_W, EMU_H);

	view_set_mask(round);
	input_init();
	input_set_hit_fn(view_hit);
	{
		int ntouch = 0;
		SDL_free(SDL_GetTouchDevices(&ntouch));
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
		input_set_default_touch(true);
		(void) forceTouch;
#else
		input_set_default_touch(round || forceTouch || ntouch > 0);
#endif
	}
	relayout();

	// Paths: ROM next to the executable/arg, saves in the per-user pref dir.
	char* pref = SDL_GetPrefPath(APP_ORG, APP_NAME);
	SDL_strlcpy(A.base, pref ? pref : "./", sizeof(A.base));
	SDL_free(pref);
	SDL_snprintf(A.savePath, sizeof(A.savePath), "%ssmbjr.sav", A.base);
	SDL_snprintf(A.statePath, sizeof(A.statePath), "%ssmbjr.ss1", A.base);

	if (romArg) {
		SDL_strlcpy(A.romPath, romArg, sizeof(A.romPath));
	} else {
		SDL_snprintf(A.romPath, sizeof(A.romPath), "%srom.gba", A.base);
		if (!file_exists(A.romPath) && file_exists("rom.gba")) {
			SDL_strlcpy(A.romPath, "rom.gba", sizeof(A.romPath));
		}
	}

#ifdef HAS_ROM_PICKER
	s_pickLock = SDL_CreateMutex();
#endif
	if (!try_load_rom()) {
		SDL_Log("%s", A.err);
		SDL_Log("Escolha a ROM na janela (clique, Ctrl+O ou arraste o .gba) ou copie para: %srom.gba", A.base);
	}

	SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, EMU_AUDIO_RATE };
	A.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (A.audio) {
		SDL_ResumeAudioStreamDevice(A.audio);
	} else {
		SDL_Log("audio: %s", SDL_GetError());
	}

	usage();
	view_toast(round ? "M: dedo ou giroscopio" : "F1: dedo ou giroscopio");
	A.lastNs = A.fpsStartNs = SDL_GetTicksNS();
	return SDL_APP_CONTINUE;
}

#ifdef __EMSCRIPTEN__
// Called from the web shell (gyro button after the motion permission prompt).
EMSCRIPTEN_KEEPALIVE void smbjr_set_control_mode(int mode) {
	if (mode >= 0 && mode < CTRL_COUNT) {
		input_set_mode((ControlMode) mode);
		char buf[64];
		SDL_snprintf(buf, sizeof(buf), "Controle: %s", input_mode_name((ControlMode) mode));
		view_toast(buf);
	}
}

EMSCRIPTEN_KEEPALIVE void smbjr_calibrate(void) {
	input_calibrate();
}

// Automation hook (tests): hold GBA key bits for some frames.
EMSCRIPTEN_KEEPALIVE void smbjr_debug_press(int keys, int frames) {
	input_debug_press((uint16_t) keys, frames);
}

// The page owns the layout (toolbar on top of the canvas): it reports the viewport size here.
EMSCRIPTEN_KEEPALIVE void smbjr_resize(int w, int h) {
	if (A.win && w > 0 && h > 0) {
		SDL_SetWindowSize(A.win, w, h);
	}
}
#endif

static void cycle_mode(void) {
	view_toast(input_cycle_mode());
}

// Hands the stage HUD to the view; when a stage starts, the tilt neutral is retaken from how
// the watch is held right then (raised like checking the time).
static void update_hud(void) {
	static bool wasHud;
	bool hud = emu_hud_active();
	if (hud && !wasHud && input_mode() != CTRL_TOUCH) {
		input_calibrate_quiet();
	}
	wasHud = hud;
	view_set_hud_layers(hud ? emu_hud_layers() : NULL);
}

static void set_paused(bool p) {
	A.paused = p;
	if (A.audio) {
		if (p) {
			SDL_PauseAudioStreamDevice(A.audio);
			SDL_ClearAudioStream(A.audio);
		} else {
			SDL_ResumeAudioStreamDevice(A.audio);
		}
	}
	A.lastNs = SDL_GetTicksNS();
	A.accNs = 0;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* e) {
	input_handle_event(e);
	switch (e->type) {
	case SDL_EVENT_QUIT:
	case SDL_EVENT_TERMINATING:
		return SDL_APP_SUCCESS;
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_RESIZED:
		relayout();
		break;
	case SDL_EVENT_WILL_ENTER_BACKGROUND:
	case SDL_EVENT_DID_ENTER_BACKGROUND:
		set_paused(true);
		break;
	case SDL_EVENT_DID_ENTER_FOREGROUND:
		set_paused(false);
		break;
#ifdef HAS_ROM_PICKER
	case SDL_EVENT_DROP_FILE:
		if (e->drop.data) import_rom(e->drop.data);
		break;
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (!A.running && e->button.button == SDL_BUTTON_LEFT) open_rom_dialog();
		break;
#endif
	case SDL_EVENT_MOUSE_WHEEL:
		// Rotary crown / bezel / mouse wheel: zoom
		view_zoom(e->wheel.y * 0.08f);
		break;
	case SDL_EVENT_KEY_DOWN:
		if (e->key.repeat) {
			break;
		}
		switch (e->key.key) {
#ifdef HAS_ROM_PICKER
		case SDLK_O:
			if (e->key.mod & SDL_KMOD_CTRL) open_rom_dialog();
			break;
		case SDLK_RETURN:
		case SDLK_SPACE:
			if (!A.running) open_rom_dialog();
			break;
#endif
		case SDLK_F1: cycle_mode(); break;
		case SDLK_F2: input_calibrate(); break;
		case SDLK_F3: view_toast(view_cycle_fit()); break;
		case SDLK_F4: view_set_overlay(!view_overlay()); break;
		case SDLK_F5:
			if (A.running) view_toast(emu_save_state(A.statePath) ? "Estado salvo" : "Falha ao salvar estado");
			break;
		case SDLK_F7:
			if (A.running) view_toast(emu_load_state(A.statePath) ? "Estado carregado" : "Sem estado salvo");
			break;
		case SDLK_F6:
			view_set_smooth(!view_smooth());
			view_toast(view_smooth() ? "Filtro: suave" : "Filtro: pixel");
			break;
		case SDLK_F8:
			input_set_pwm(!input_pwm());
			view_toast(input_pwm() ? "Analogico PWM: ligado" : "Analogico PWM: desligado");
			break;
		case SDLK_F10:
			recomp_set_enabled(!recomp_enabled());
			view_toast(recomp_enabled() ? "Codigo recompilado (nativo)" : "Interpretador mGBA");
			break;
		case SDLK_F9:
			A.showDebug = !A.showDebug;
			if (!A.showDebug) view_set_debug(NULL);
			break;
		case SDLK_F11: {
			bool fs = (SDL_GetWindowFlags(A.win) & SDL_WINDOW_FULLSCREEN) != 0;
			SDL_SetWindowFullscreen(A.win, !fs);
			break;
		}
		case SDLK_P: set_paused(!A.paused); view_toast(A.paused ? "Pausado" : "Continuar"); break;
		case SDLK_TAB: A.fast = true; break;
		case SDLK_ESCAPE:
#ifndef SDL_PLATFORM_ANDROID
			return SDL_APP_SUCCESS;
#else
			break;
#endif
		default: break;
		}
		break;
	case SDL_EVENT_KEY_UP:
		if (e->key.key == SDLK_TAB) A.fast = false;
		break;
	default:
		break;
	}
	return SDL_APP_CONTINUE;
}

static void run_emulation(void) {
	uint64_t now = SDL_GetTicksNS();
	double dt = (double) (now - A.lastNs);
	A.lastNs = now;
	if (A.paused) {
		return;
	}
	A.accNs += dt * (A.fast ? 4.0 : 1.0);
	if (A.accNs > A.frameNs * 6) {
		A.accNs = A.frameNs;  // fell far behind (debugger, suspend): don't spiral
	}

	static int16_t samples[4096 * 2];
	int ran = 0;
	while (A.accNs >= A.frameNs && ran < (A.fast ? 8 : 3)) {
		A.accNs -= A.frameNs;
		emu_run_frame(input_frame());
		++ran;
		int n = emu_read_audio(samples, 4096);
		if (A.audio && n > 0 && !A.fast) {
			int queued = SDL_GetAudioStreamQueued(A.audio) / 4;
			if (queued < AUDIO_MAX_QUEUE_FRAMES) {
				SDL_PutAudioStreamData(A.audio, samples, n * 4);
			}
		}
	}
	if (ran) {
		SDL_UpdateTexture(A.tex, NULL, emu_framebuffer(), EMU_W * 4);
		A.fpsFrames += ran;
	}
	if (now - A.fpsStartNs >= 1000000000ull) {
		A.fpsValue = A.fpsFrames * 1e9 / (double) (now - A.fpsStartNs);
		A.fpsFrames = 0;
		A.fpsStartNs = now;
	}
	if (A.showDebug) {
		float ax, ay;
		input_analog(&ax, &ay);
		uint16_t dc = emu_dispcnt();
		char buf[128];
		SDL_snprintf(buf, sizeof(buf), "%.1ffps mode%d BG%c%c%c%c OBJ%c  %+.2f %+.2f", A.fpsValue, dc & 7,
		             dc & 0x100 ? '0' : '-', dc & 0x200 ? '1' : '-', dc & 0x400 ? '2' : '-',
		             dc & 0x800 ? '3' : '-', dc & 0x1000 ? '+' : '-', ax, ay);
		view_set_debug(buf);
	}
}

// Scripted autoplay used for automated verification on desktop.
static uint16_t test_keys(int f) {
	if (f >= 240 && f < 2400 && f % 90 < 4) {
		return (f / 90) % 2 ? KEY_START : KEY_A;
	}
	if (f >= 2400) {
		return (f / 30) % 3 ? KEY_UP : 0;
	}
	return 0;
}

static void test_shot(void) {
	SDL_Surface* s = SDL_RenderReadPixels(A.ren, NULL);
	if (s) {
		char path[256];
		SDL_snprintf(path, sizeof(path), "shots/f%05d.bmp", A.frameNo);
		SDL_SaveBMP(s, path);
		SDL_DestroySurface(s);
	}
	SDL_Surface* raw = SDL_CreateSurfaceFrom(EMU_W, EMU_H, SDL_PIXELFORMAT_XBGR8888, (void*) emu_framebuffer(), EMU_W * 4);
	if (raw) {
		char path[256];
		SDL_snprintf(path, sizeof(path), "shots/raw%05d.bmp", A.frameNo);
		SDL_SaveBMP(raw, path);
		SDL_DestroySurface(raw);
	}
	uint16_t dc = emu_dispcnt();
	SDL_Log("frame %d DISPCNT=%04X mode=%d", A.frameNo, dc, dc & 7);
}

static SDL_AppResult test_iterate(void) {
	static int16_t samples[4096 * 2];
	bool shot = false;
	for (int i = 0; i < 10 && A.frameNo < A.testFrames; ++i) {
		emu_run_frame(test_keys(A.frameNo));
		if (A.hashLog) {
			SDL_IOprintf(A.hashLog, "%d %016llx\n", A.frameNo, (unsigned long long) emu_state_hash());
		}
		{
			static int last = -1;
			int act = emu_hud_active();
			if (act != last) {
				SDL_Log("frame %d hud_active=%d", A.frameNo, act);
				last = act;
			}
		}
		emu_read_audio(samples, 4096);
		++A.frameNo;
		if (A.frameNo % 150 == 0) {
			shot = true;
			break;
		}
	}
	SDL_UpdateTexture(A.tex, NULL, emu_framebuffer(), EMU_W * 4);
	update_hud();
	view_render(A.ren, A.tex);
	if (shot) {
		test_shot();
	}
	SDL_RenderPresent(A.ren);
	return A.frameNo >= A.testFrames ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
#ifdef __EMSCRIPTEN__
	static unsigned iters;
	if (++iters % 600 == 1) {
		SDL_Log("iterate %u running=%d paused=%d", iters, A.running, A.paused);
	}
#endif
#ifdef HAS_ROM_PICKER
	poll_picked_rom();
#endif
	if (A.running && A.testFrames) {
		return test_iterate();
	}
	if (A.running) {
		run_emulation();
		update_hud();
		view_render(A.ren, A.tex);
	} else {
		static int poll;
		if (++poll % 30 == 0 && try_load_rom()) {
			return SDL_APP_CONTINUE;
		}
		SDL_SetRenderDrawColor(A.ren, 0, 0, 0, 255);
		SDL_RenderClear(A.ren);
		int w, h;
		SDL_GetRenderOutputSize(A.ren, &w, &h);
		// 8 px debug font: scale it up so it stays readable on large / high-DPI windows.
		float s = SDL_max(1.0f, SDL_floorf(SDL_min(w / 360.0f, h / 180.0f)));
		SDL_SetRenderScale(A.ren, s, s);
		float x = 16 / s, y = h / s * 0.35f;
		SDL_SetRenderDrawColor(A.ren, 250, 199, 46, 255);
		SDL_RenderDebugText(A.ren, x, y, "Super Recomp Ball Jr.");
		SDL_SetRenderDrawColor(A.ren, 255, 255, 255, 255);
#ifdef __EMSCRIPTEN__
		SDL_RenderDebugText(A.ren, x, y + 20, "Escolha o arquivo .gba (botao ROM acima).");
#elif defined(HAS_ROM_PICKER)
		SDL_RenderDebugText(A.ren, x, y + 20, "Clique aqui (ou Ctrl+O) para escolher");
		SDL_RenderDebugText(A.ren, x, y + 32, "o arquivo .gba, ou arraste-o para a janela.");
#else
		SDL_RenderDebugText(A.ren, x, y + 20, "ROM nao encontrada. Copie para:");
		SDL_RenderDebugText(A.ren, x, y + 32, A.base);
		SDL_RenderDebugText(A.ren, x, y + 44, "com o nome rom.gba");
#endif
		const char* msg = A.note[0] ? A.note : (SDL_strcmp(A.err, "ROM nao encontrada") ? A.err : "");
		if (msg[0]) {
			SDL_SetRenderDrawColor(A.ren, 255, 115, 102, 255);
			SDL_RenderDebugText(A.ren, x, y + 64, msg);
		}
		SDL_SetRenderScale(A.ren, 1, 1);
	}
	SDL_RenderPresent(A.ren);
	return SDL_APP_CONTINUE;
}

static void recomp_report(void) {
	if (!recomp_available()) {
		return;
	}
	struct RecompStats st;
	recomp_stats(&st);
	SDL_Log("recomp: %s, %d instructions; %llu native runs, %llu interpreted instructions at %u addresses, %u guard misses",
	        recomp_enabled() ? "native" : "interpreter", st.insnCount, (unsigned long long) st.nativeRuns,
	        (unsigned long long) st.interpreted, st.missAddresses, st.guardFails);
	if (A.missesPath && recomp_write_misses(A.missesPath)) {
		SDL_Log("recomp: misses written to %s", A.missesPath);
	}
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
	if (A.hashLog) {
		SDL_CloseIO(A.hashLog);
	}
	if (A.running) {
		recomp_report();
		emu_shutdown();
	}
	input_shutdown();
	if (A.audio) {
		SDL_DestroyAudioStream(A.audio);
	}
}
