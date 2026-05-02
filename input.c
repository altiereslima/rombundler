/*
 * input.c — Sistema de entrada para ROMBundler.
 *
 * Mapeamento padrão: Xbox 360 (A=A, B=B, X=X, Y=Y).
 * Suporta remapeamento por jogo via remap.c.
 * Atalhos: ALT+ENTER/F11 = fullscreen, F1 = menu, BACK+START = menu (gamepad).
 */

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "input.h"
#include "config.h"
#include "libretro.h"
#include "video.h"
#include "menu.h"
#include "remap.h"
#include "utils.h"

extern config g_cfg;

/* Mapeamento teclado → joypad (para jogar sem controle) */
struct keymap {
	unsigned k;  /* Tecla GLFW */
	unsigned rk; /* Botão libretro */
};

struct keymap kbd2joy_binds[] = {
	{ GLFW_KEY_X, RETRO_DEVICE_ID_JOYPAD_A },
	{ GLFW_KEY_Z, RETRO_DEVICE_ID_JOYPAD_B },
	{ GLFW_KEY_A, RETRO_DEVICE_ID_JOYPAD_Y },
	{ GLFW_KEY_S, RETRO_DEVICE_ID_JOYPAD_X },
	{ GLFW_KEY_UP, RETRO_DEVICE_ID_JOYPAD_UP },
	{ GLFW_KEY_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN },
	{ GLFW_KEY_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT },
	{ GLFW_KEY_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT },
	{ GLFW_KEY_ENTER, RETRO_DEVICE_ID_JOYPAD_START },
	{ GLFW_KEY_RIGHT_SHIFT, RETRO_DEVICE_ID_JOYPAD_SELECT },
	{ GLFW_KEY_Q, RETRO_DEVICE_ID_JOYPAD_L },
	{ GLFW_KEY_W, RETRO_DEVICE_ID_JOYPAD_R },
	{ GLFW_KEY_1, RETRO_DEVICE_ID_JOYPAD_L2 },
	{ GLFW_KEY_2, RETRO_DEVICE_ID_JOYPAD_R2 },
};

/* Mapeamento teclado completo → libretro keyboard */
struct keymap kbd_binds[] = {
	{ GLFW_KEY_BACKSPACE, RETROK_BACKSPACE },
	{ GLFW_KEY_TAB, RETROK_TAB },
	{ GLFW_KEY_ENTER, RETROK_RETURN },
	{ GLFW_KEY_PAUSE, RETROK_PAUSE },
	{ GLFW_KEY_ESCAPE, RETROK_ESCAPE },
	{ GLFW_KEY_SPACE, RETROK_SPACE },
	{ GLFW_KEY_COMMA, RETROK_COMMA },
	{ GLFW_KEY_MINUS, RETROK_MINUS },
	{ GLFW_KEY_PERIOD, RETROK_PERIOD },
	{ GLFW_KEY_SLASH, RETROK_SLASH },
	{ GLFW_KEY_0, RETROK_0 },
	{ GLFW_KEY_1, RETROK_1 },
	{ GLFW_KEY_2, RETROK_2 },
	{ GLFW_KEY_3, RETROK_3 },
	{ GLFW_KEY_4, RETROK_4 },
	{ GLFW_KEY_5, RETROK_5 },
	{ GLFW_KEY_6, RETROK_6 },
	{ GLFW_KEY_7, RETROK_7 },
	{ GLFW_KEY_8, RETROK_8 },
	{ GLFW_KEY_9, RETROK_9 },
	{ GLFW_KEY_COMMA, RETROK_COLON },
	{ GLFW_KEY_SEMICOLON, RETROK_SEMICOLON },
	{ GLFW_KEY_MINUS, RETROK_LESS },
	{ GLFW_KEY_EQUAL, RETROK_EQUALS },
	{ GLFW_KEY_LEFT_BRACKET, RETROK_LEFTBRACKET },
	{ GLFW_KEY_BACKSLASH, RETROK_BACKSLASH },
	{ GLFW_KEY_RIGHT_BRACKET, RETROK_RIGHTBRACKET },
	{ GLFW_KEY_GRAVE_ACCENT, RETROK_BACKQUOTE },
	{ GLFW_KEY_A, RETROK_a },
	{ GLFW_KEY_B, RETROK_b },
	{ GLFW_KEY_C, RETROK_c },
	{ GLFW_KEY_D, RETROK_d },
	{ GLFW_KEY_E, RETROK_e },
	{ GLFW_KEY_F, RETROK_f },
	{ GLFW_KEY_G, RETROK_g },
	{ GLFW_KEY_H, RETROK_h },
	{ GLFW_KEY_I, RETROK_i },
	{ GLFW_KEY_J, RETROK_j },
	{ GLFW_KEY_K, RETROK_k },
	{ GLFW_KEY_L, RETROK_l },
	{ GLFW_KEY_M, RETROK_m },
	{ GLFW_KEY_N, RETROK_n },
	{ GLFW_KEY_O, RETROK_o },
	{ GLFW_KEY_P, RETROK_p },
	{ GLFW_KEY_Q, RETROK_q },
	{ GLFW_KEY_R, RETROK_r },
	{ GLFW_KEY_S, RETROK_s },
	{ GLFW_KEY_T, RETROK_t },
	{ GLFW_KEY_U, RETROK_u },
	{ GLFW_KEY_V, RETROK_v },
	{ GLFW_KEY_W, RETROK_w },
	{ GLFW_KEY_X, RETROK_x },
	{ GLFW_KEY_Y, RETROK_y },
	{ GLFW_KEY_Z, RETROK_z },
	{ GLFW_KEY_LEFT_BRACKET, RETROK_LEFTBRACE },
	{ GLFW_KEY_RIGHT_BRACKET, RETROK_RIGHTBRACE },
	{ GLFW_KEY_DELETE, RETROK_DELETE },

	{ GLFW_KEY_KP_0, RETROK_KP0 },
	{ GLFW_KEY_KP_1, RETROK_KP1 },
	{ GLFW_KEY_KP_2, RETROK_KP2 },
	{ GLFW_KEY_KP_3, RETROK_KP3 },
	{ GLFW_KEY_KP_4, RETROK_KP4 },
	{ GLFW_KEY_KP_5, RETROK_KP5 },
	{ GLFW_KEY_KP_6, RETROK_KP6 },
	{ GLFW_KEY_KP_7, RETROK_KP7 },
	{ GLFW_KEY_KP_8, RETROK_KP8 },
	{ GLFW_KEY_KP_9, RETROK_KP9 },
	{ GLFW_KEY_KP_DECIMAL, RETROK_KP_PERIOD },
	{ GLFW_KEY_KP_DIVIDE, RETROK_KP_DIVIDE },
	{ GLFW_KEY_KP_MULTIPLY, RETROK_KP_MULTIPLY },
	{ GLFW_KEY_KP_SUBTRACT, RETROK_KP_MINUS },
	{ GLFW_KEY_KP_ADD, RETROK_KP_PLUS },
	{ GLFW_KEY_KP_ENTER, RETROK_KP_ENTER },
	{ GLFW_KEY_KP_EQUAL, RETROK_KP_EQUALS },

	{ GLFW_KEY_UP, RETROK_UP },
	{ GLFW_KEY_DOWN, RETROK_DOWN },
	{ GLFW_KEY_RIGHT, RETROK_RIGHT },
	{ GLFW_KEY_LEFT, RETROK_LEFT },
	{ GLFW_KEY_INSERT, RETROK_INSERT },
	{ GLFW_KEY_HOME, RETROK_HOME },
	{ GLFW_KEY_END, RETROK_END },
	{ GLFW_KEY_PAGE_UP, RETROK_PAGEUP },
	{ GLFW_KEY_PAGE_DOWN, RETROK_PAGEDOWN },

	{ GLFW_KEY_F1, RETROK_F1 },
	{ GLFW_KEY_F2, RETROK_F2 },
	{ GLFW_KEY_F3, RETROK_F3 },
	{ GLFW_KEY_F4, RETROK_F4 },
	{ GLFW_KEY_F5, RETROK_F5 },
	{ GLFW_KEY_F6, RETROK_F6 },
	{ GLFW_KEY_F7, RETROK_F7 },
	{ GLFW_KEY_F8, RETROK_F8 },
	{ GLFW_KEY_F9, RETROK_F9 },
	{ GLFW_KEY_F10, RETROK_F10 },
	{ GLFW_KEY_F11, RETROK_F11 },
	{ GLFW_KEY_F12, RETROK_F12 },
	{ GLFW_KEY_F13, RETROK_F13 },
	{ GLFW_KEY_F14, RETROK_F14 },
	{ GLFW_KEY_F15, RETROK_F15 },

	{ GLFW_KEY_NUM_LOCK, RETROK_NUMLOCK },
	{ GLFW_KEY_CAPS_LOCK, RETROK_CAPSLOCK },
	{ GLFW_KEY_SCROLL_LOCK, RETROK_SCROLLOCK },
	{ GLFW_KEY_RIGHT_SHIFT, RETROK_RSHIFT },
	{ GLFW_KEY_LEFT_SHIFT, RETROK_LSHIFT },
	{ GLFW_KEY_RIGHT_CONTROL, RETROK_RCTRL },
	{ GLFW_KEY_LEFT_CONTROL, RETROK_LCTRL },
	{ GLFW_KEY_RIGHT_ALT, RETROK_RALT },
	{ GLFW_KEY_LEFT_ALT, RETROK_LALT },
	{ GLFW_KEY_LEFT_SUPER, RETROK_LSUPER },
	{ GLFW_KEY_RIGHT_SUPER, RETROK_RSUPER },
	{ GLFW_KEY_PRINT_SCREEN, RETROK_PRINT },
	{ GLFW_KEY_MENU, RETROK_MENU },

	{ 0, 0 }
};

#define MAX_PLAYERS 5
static int16_t state[MAX_PLAYERS][RETRO_DEVICE_ID_JOYPAD_R3+1] = { 0 };
static int16_t analog_state[MAX_PLAYERS][2][2] = { 0 };
static retro_keyboard_event_t key_event = NULL;
static int active_gamepads[MAX_PLAYERS] = { -1, -1, -1, -1, -1 };

static void input_refresh_active_gamepads(void)
{
	int count = 0;

	for (int port = 0; port < MAX_PLAYERS; port++)
		active_gamepads[port] = -1;

	for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST && count < MAX_PLAYERS; jid++) {
		if (!glfwJoystickPresent(jid))
			continue;
		if (!glfwJoystickIsGamepad(jid))
			continue;
		active_gamepads[count++] = jid;
	}
}

static int input_gamepad_jid_for_port(int port)
{
	if (port < 0 || port >= MAX_PLAYERS)
		return -1;
	return active_gamepads[port];
}
static bool ff_active = false;
static bool ff_active_prev = false;
static bool mouse_capture_active = false;
static double oldx = 0;
static double oldy = 0;
static double mouse_requested_at = -1.0;
extern GLFWwindow *window;

#define MOUSE_CAPTURE_GRACE 0.50

/* Debounce para atalhos (ALT+ENTER, F1, BACK+START) */
static double hotkey_debounce = 0;
#define HOTKEY_DELAY 0.3
#define MENU_COMBO_HOLD 0.4
static bool hotkey_alt_enter_prev = false;
static bool hotkey_f11_prev = false;
static bool hotkey_f1_prev = false;
static bool hotkey_escape_prev = false;
static double menu_combo_hold_start[MAX_PLAYERS] = { 0.0 };
static bool menu_combo_triggered[MAX_PLAYERS] = { false };

static bool hotkey_triggered(bool pressed, bool *prev_pressed)
{
	double now = glfwGetTime();
	bool triggered = pressed && !*prev_pressed;

	*prev_pressed = pressed;
	if (!triggered)
		return false;
	if (now - hotkey_debounce < HOTKEY_DELAY)
		return false;
	hotkey_debounce = now;
	return true;
}

static bool ff_gamepad_button_pressed(void)
{
	if (g_cfg.ff_button < 0 || g_cfg.ff_button > RETRO_DEVICE_ID_JOYPAD_R3)
		return false;

	for (int port = 0; port < MAX_PLAYERS; port++) {
		int jid = input_gamepad_jid_for_port(port);
		GLFWgamepadstate pad;
		unsigned mapped_btn = 0;

		if (jid < 0)
			continue;
		if (!glfwGetGamepadState(jid, &pad))
			continue;

		switch (g_cfg.ff_button) {
			case RETRO_DEVICE_ID_JOYPAD_L2:
				if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.5f)
					return true;
				continue;
			case RETRO_DEVICE_ID_JOYPAD_R2:
				if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.5f)
					return true;
				continue;
			default:
				mapped_btn = remap_get(port, (unsigned)g_cfg.ff_button);
				if (mapped_btn <= GLFW_GAMEPAD_BUTTON_LAST &&
				    pad.buttons[mapped_btn] == GLFW_PRESS)
					return true;
				break;
		}
	}

	return false;
}

int16_t floatToAnalog(float v) {
	return (int16_t)(v * 32767.0f);
}

bool input_is_fast_forward(void) {
	return ff_active;
}

static bool input_uses_mouse_device(void)
{
	return g_cfg.port0 == RETRO_DEVICE_MOUSE ||
	       g_cfg.port1 == RETRO_DEVICE_MOUSE ||
	       g_cfg.port2 == RETRO_DEVICE_MOUSE ||
	       g_cfg.port3 == RETRO_DEVICE_MOUSE;
}

static void update_mouse_capture(void)
{
	bool want_capture = false;
	bool mouse_requested_recently = false;

	if (!window)
		return;

	mouse_requested_recently =
		mouse_requested_at >= 0.0 &&
		(glfwGetTime() - mouse_requested_at) < MOUSE_CAPTURE_GRACE;
	want_capture =
		(input_uses_mouse_device() || mouse_requested_recently) &&
		(!video_menu_supported() || !menu_is_active());
	if (want_capture == mouse_capture_active)
		return;

	if (want_capture)
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	else
		video_apply_cursor_mode();

	glfwGetCursorPos(window, &oldx, &oldy);
	mouse_capture_active = want_capture;
}

void input_poll(void) {
	int i;
	int port;
	bool alt_enter_pressed;
	bool menu_supported;

	if (!window) return;

	menu_supported = video_menu_supported();

	input_refresh_active_gamepads();

	/* ─── Atalhos globais (funcionam sempre, com ou sem menu) ─── */

	/* ALT+ENTER = alternar tela cheia */
	alt_enter_pressed =
		(glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
		 glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS) &&
		glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS;
	if (hotkey_triggered(alt_enter_pressed, &hotkey_alt_enter_prev)) {
		log_printf("input", "ALT+ENTER hotkey triggered");
		video_toggle_fullscreen();
		return; /* Não processa mais nada nesse frame */
	}

	/* F11 = alternar tela cheia (alternativa ao ALT+ENTER) */
	if (hotkey_triggered(glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS,
		&hotkey_f11_prev)) {
		log_printf("input", "F11 hotkey triggered");
		video_toggle_fullscreen();
		return;
	}

	/* F1 = abrir/fechar menu */
	if (menu_supported &&
	    hotkey_triggered(glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS,
		&hotkey_f1_prev)) {
		log_printf("input", "F1 hotkey triggered");
		menu_toggle();
		return;
	}

	/* BACK+START no gamepad = abrir/fechar menu (requer segurar 0.4s) */
	for (int port = 0; port < MAX_PLAYERS; port++) {
		bool combo_pressed = false;
		int jid = input_gamepad_jid_for_port(port);
		GLFWgamepadstate pad;
		double now = glfwGetTime();

		if (jid >= 0 &&
		    glfwGetGamepadState(jid, &pad)) {
			combo_pressed =
				pad.buttons[GLFW_GAMEPAD_BUTTON_BACK] == GLFW_PRESS &&
				pad.buttons[GLFW_GAMEPAD_BUTTON_START] == GLFW_PRESS;
		}

		if (menu_supported && combo_pressed) {
			if (!menu_combo_triggered[port] && menu_combo_hold_start[port] == 0.0)
				menu_combo_hold_start[port] = now;
			if (!menu_combo_triggered[port] &&
			    now - menu_combo_hold_start[port] >= MENU_COMBO_HOLD &&
			    now - hotkey_debounce >= HOTKEY_DELAY) {
				hotkey_debounce = now;
				menu_combo_triggered[port] = true;
				log_printf("input", "BACK+START hotkey triggered on port %d (held %.2fs)", port, now - menu_combo_hold_start[port]);
				menu_toggle();
				return;
			}
		} else {
			menu_combo_hold_start[port] = 0.0;
			menu_combo_triggered[port] = false;
		}
	}

	/* Se o menu está ativo, não atualiza o estado do jogo */
	ff_active =
		(glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) ||
		ff_gamepad_button_pressed();

	if (ff_active != ff_active_prev) {
		bool space_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
		bool gamepad_pressed = ff_gamepad_button_pressed();
		log_printf("input", "fast-forward active=%d source=%s%s",
			ff_active ? 1 : 0,
			space_pressed ? "space" : "",
			gamepad_pressed ? (space_pressed ? "+gamepad" : "gamepad") : "");
		ff_active_prev = ff_active;
	}

	update_mouse_capture();

	if (menu_supported && menu_is_active())
		return;

	/* ─── Entrada normal do jogo ─── */

	for (port = 0; port < MAX_PLAYERS; port++) {
		memset(state[port], 0, sizeof(state[port]));
		memset(analog_state[port], 0, sizeof(analog_state[port]));
	}

	if (key_event) {
		for (i = 0; kbd_binds[i].k || kbd_binds[i].rk; ++i) {
			bool pressed = glfwGetKey(window, kbd_binds[i].k) == GLFW_PRESS;
			key_event(pressed, kbd_binds[i].rk, 0, 0);
		}
	} else {
		for (i = 0; i < 14; i++)
			state[0][kbd2joy_binds[i].rk] = glfwGetKey(window, kbd2joy_binds[i].k) == GLFW_PRESS;

		/* ESC abre o menu ao invés de fechar a janela */
		if (menu_supported &&
		    hotkey_triggered(glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS,
			&hotkey_escape_prev)) {
			log_printf("input", "ESC hotkey triggered");
			menu_toggle();
		}
	}

	/* Gamepads — usa o sistema de remap */
	for (port = 0; port < MAX_PLAYERS; port++) {
		int jid = input_gamepad_jid_for_port(port);

		if (jid < 0)
			continue;

		GLFWgamepadstate pad;
		if (glfwGetGamepadState(jid, &pad)) {
			/* Botões de ação via remap (14 primeiros botões: A,B,X,Y,UP,DOWN,LEFT,RIGHT,START,SELECT,L,R,L3,R3) */
			for (i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; i++) {
				unsigned mapped_btn = remap_get(port, i);
				if (i == RETRO_DEVICE_ID_JOYPAD_L2 || i == RETRO_DEVICE_ID_JOYPAD_R2)
					continue; /* L2/R2 são eixos, tratados abaixo */
				if (mapped_btn <= GLFW_GAMEPAD_BUTTON_LAST)
					state[port][i] = pad.buttons[mapped_btn];
			}

			/* Analógicos */
			analog_state[port][RETRO_DEVICE_INDEX_ANALOG_LEFT][RETRO_DEVICE_ID_ANALOG_X] = floatToAnalog(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
			analog_state[port][RETRO_DEVICE_INDEX_ANALOG_LEFT][RETRO_DEVICE_ID_ANALOG_Y] = floatToAnalog(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
			analog_state[port][RETRO_DEVICE_INDEX_ANALOG_RIGHT][RETRO_DEVICE_ID_ANALOG_X] = floatToAnalog(pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
			analog_state[port][RETRO_DEVICE_INDEX_ANALOG_RIGHT][RETRO_DEVICE_ID_ANALOG_Y] = floatToAnalog(pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);

			/* L2/R2: gatilhos analógicos com remap opcional para outro botão retro */
			if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.5f)
				state[port][remap_get_trigger_l2_target(port)] = 1;
			if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.5f)
				state[port][remap_get_trigger_r2_target(port)] = 1;

			/* Analógico para D-pad: modo por porta (-1=herda global, 0=off, 1=esq, 2=dir, 3=ambos) */
			int adpad = remap_get_analog_dpad_mode(port);
			if (adpad < 0) adpad = g_cfg.map_analog_to_dpad ? 1 : 0;
			if (adpad == 1 || adpad == 3) {
				if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X] < -0.5f) state[port][RETRO_DEVICE_ID_JOYPAD_LEFT]  = 1;
				if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X] >  0.5f) state[port][RETRO_DEVICE_ID_JOYPAD_RIGHT] = 1;
				if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.5f) state[port][RETRO_DEVICE_ID_JOYPAD_UP]    = 1;
				if (pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] >  0.5f) state[port][RETRO_DEVICE_ID_JOYPAD_DOWN]  = 1;
			}
			if (adpad == 2 || adpad == 3) {
				if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] < -0.5f) state[port][RETRO_DEVICE_ID_JOYPAD_LEFT]  = 1;
				if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] >  0.5f) state[port][RETRO_DEVICE_ID_JOYPAD_RIGHT] = 1;
				if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] < -0.5f) state[port][RETRO_DEVICE_ID_JOYPAD_UP]    = 1;
				if (pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] >  0.5f) state[port][RETRO_DEVICE_ID_JOYPAD_DOWN]  = 1;
			}
		}
	}
}

void input_set_keyboard_callback(retro_keyboard_event_t e)
{
	key_event = e;
}

int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	/* Quando o menu está ativo, bloqueia toda entrada pro jogo */
	if (video_menu_supported() && menu_is_active())
		return 0;

	if (port >= MAX_PLAYERS)
		return 0;

	if (device == RETRO_DEVICE_JOYPAD) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
			int16_t mask = 0;
			for (unsigned i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; i++) {
				if (state[port][i])
					mask |= (int16_t)(1u << i);
			}
			return mask;
		}

		if (id > RETRO_DEVICE_ID_JOYPAD_R3)
			return 0;
		return state[port][id];
	}

	if (device == RETRO_DEVICE_ANALOG) {
		if (index > RETRO_DEVICE_INDEX_ANALOG_RIGHT ||
		    id > RETRO_DEVICE_ID_ANALOG_Y)
			return 0;
		return analog_state[port][index][id];
	}

	if (device == RETRO_DEVICE_MOUSE && window != NULL) {
		mouse_requested_at = glfwGetTime();
		double x = 0;
		double y = 0;
		glfwGetCursorPos(window, &x, &y);
		if (id == RETRO_DEVICE_ID_MOUSE_X) {
			int16_t d = (int16_t)(x - oldx);
			oldx = x;
			return d;
		}
		if (id == RETRO_DEVICE_ID_MOUSE_Y) {
			int16_t d = (int16_t)(y - oldy);
			oldy = y;
			return d;
		}
		if (id == RETRO_DEVICE_ID_MOUSE_LEFT && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_1) == GLFW_PRESS)
			return 1;
	}

	if (device == RETRO_DEVICE_KEYBOARD)
		for (int j = 0; kbd_binds[j].k || kbd_binds[j].rk; ++j)
			if (id == kbd_binds[j].rk && window && glfwGetKey(window, kbd_binds[j].k) == GLFW_PRESS)
				return 1;

	return 0;
}
