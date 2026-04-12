/*
 * menu.c — Overlay menu system for ROMBundler.
 *
 * Renders an immediate-mode style overlay (inspired by nes-bundler's egui menu)
 * on top of the game frame. Navigable by keyboard (arrows + Enter + Esc) and
 * gamepad (D-pad + A/B + Start).
 *
 * Visual style: dark semi-transparent background, monospace white text for
 * selected items, gray for unselected, matching nes-bundler's MenuButton widget.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "menu.h"
#include "font.h"
#include "config.h"
#include "options.h"
#include "video.h"
#include "remap.h"
#include "libretro.h"
#include "lang.h"
#include "aspect.h"
#include "audio.h"
#include "utils.h"

extern config g_cfg;
extern GLFWwindow *window;

/* ─────────────────── Menu State ─────────────────── */

static bool       menu_active = false;
static menu_page_t menu_page  = MENU_PAGE_MAIN;
static int        menu_sel    = 0;
static int        menu_scroll = 0;

/* Debounce: prevent repeat inputs */
static double     last_input_time = 0;
#define INPUT_REPEAT_DELAY 0.15

/* Remap capture state */
static bool       remap_capturing = false;
static bool       remap_wait_release = false;  /* Wait for all buttons released before capturing */
static int        remap_capture_port = 0;
static int        remap_capture_button = -1;

/* ─────────────────── Helpers ─────────────────── */

static bool input_debounce(void)
{
	double now = glfwGetTime();
	if (now - last_input_time < INPUT_REPEAT_DELAY)
		return false;
	last_input_time = now;
	return true;
}

/* Check keyboard key with debounce */
static bool key_pressed(int key)
{
	return glfwGetKey(window, key) == GLFW_PRESS;
}

/* Check gamepad button */
static bool pad_button(int port, int btn)
{
	if (!glfwJoystickIsGamepad(port)) return false;
	GLFWgamepadstate pad;
	if (!glfwGetGamepadState(port, &pad)) return false;
	return pad.buttons[btn] == GLFW_PRESS;
}

/* Check gamepad axis above half */
static bool pad_axis_lt_half(int port, int axis)
{
	if (!glfwJoystickIsGamepad(port)) return false;
	GLFWgamepadstate pad;
	if (!glfwGetGamepadState(port, &pad)) return false;
	return pad.axes[axis] > 0.0f;
}

/* Navigation: up */
static bool nav_up(void)
{
	return key_pressed(GLFW_KEY_UP) || pad_button(0, GLFW_GAMEPAD_BUTTON_DPAD_UP);
}

/* Navigation: down */
static bool nav_down(void)
{
	return key_pressed(GLFW_KEY_DOWN) || pad_button(0, GLFW_GAMEPAD_BUTTON_DPAD_DOWN);
}

/* Navigation: left */
static bool nav_left(void)
{
	return key_pressed(GLFW_KEY_LEFT) || pad_button(0, GLFW_GAMEPAD_BUTTON_DPAD_LEFT);
}

/* Navigation: right */
static bool nav_right(void)
{
	return key_pressed(GLFW_KEY_RIGHT) || pad_button(0, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT);
}

static bool prev_confirm = false;
static bool nav_confirm(void)
{
	bool pressed = key_pressed(GLFW_KEY_ENTER) || pad_button(0, GLFW_GAMEPAD_BUTTON_A);
	bool triggered = pressed && !prev_confirm;
	prev_confirm = pressed;
	return triggered;
}

/* Navigation: back (Esc / B button on Xbox layout) */
static bool prev_back = false;
static bool nav_back(void)
{
	bool pressed = key_pressed(GLFW_KEY_ESCAPE) || pad_button(0, GLFW_GAMEPAD_BUTTON_B);
	bool triggered = pressed && !prev_back;
	prev_back = pressed;
	return triggered;
}

/* L/R Bumpers and Triggers for adjusting custom aspect ratio */
static bool nav_lb(void)
{
	return key_pressed(GLFW_KEY_Q) || pad_button(0, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
}

static bool nav_rb(void)
{
	return key_pressed(GLFW_KEY_W) || pad_button(0, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
}

static bool nav_lt(void)
{
	return key_pressed(GLFW_KEY_A) || pad_axis_lt_half(0, GLFW_GAMEPAD_AXIS_LEFT_TRIGGER);
}

static bool nav_rt(void)
{
	return key_pressed(GLFW_KEY_S) || pad_axis_lt_half(0, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER);
}

/* ─────────────────── SW rendering target (Vulkan) ─────────────────── */

static uint32_t *menu_sw_buf = NULL;
static int menu_sw_w = 0, menu_sw_h = 0;

void menu_set_sw_target(uint32_t *buf, int w, int h)
{
	menu_sw_buf = buf;
	menu_sw_w = w;
	menu_sw_h = h;
}

void menu_clear_sw_target(void)
{
	menu_sw_buf = NULL;
}

/* ─────────────────── Rendering Helpers ─────────────────── */

static int scr_w, scr_h;
static float scale;

/* Shader e VBOs para desenhar retângulos coloridos (overlay bg + highlight) */
static GLuint menu_shader = 0;
static GLuint menu_vao = 0;
static GLuint menu_vbo = 0;
static GLint  menu_u_color = -1;
static GLint  menu_i_pos = -1;
static bool   menu_gl_inited = false;

static const char *menu_vsrc =
	"attribute vec2 i_pos;\n"
	"void main() {\n"
	"  gl_Position = vec4(i_pos, 0.0, 1.0);\n"
	"}\n";

static const char *menu_fsrc =
	"uniform vec4 u_color;\n"
	"void main() {\n"
	"  gl_FragColor = u_color;\n"
	"}\n";

static void menu_init_gl(void)
{
	if (menu_gl_inited) return;

	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &menu_vsrc, NULL);
	glCompileShader(vs);

	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &menu_fsrc, NULL);
	glCompileShader(fs);

	menu_shader = glCreateProgram();
	glAttachShader(menu_shader, vs);
	glAttachShader(menu_shader, fs);
	glLinkProgram(menu_shader);
	glDeleteShader(vs);
	glDeleteShader(fs);

	menu_i_pos   = glGetAttribLocation(menu_shader, "i_pos");
	menu_u_color = glGetUniformLocation(menu_shader, "u_color");

	glGenVertexArrays(1, &menu_vao);
	glGenBuffers(1, &menu_vbo);

	menu_gl_inited = true;
}

static void draw_rect(float nx0, float ny0, float nx1, float ny1,
                       float r, float g, float b, float a)
{
	if (menu_sw_buf) {
		/* Software path: NDC (-1..1) -> pixel coords */
		int px0 = (int)((nx0 + 1.0f) * 0.5f * menu_sw_w);
		int py0 = (int)((1.0f - ny1) * 0.5f * menu_sw_h);
		int px1 = (int)((nx1 + 1.0f) * 0.5f * menu_sw_w);
		int py1 = (int)((1.0f - ny0) * 0.5f * menu_sw_h);
		if (px0 < 0) px0 = 0; if (px1 > menu_sw_w) px1 = menu_sw_w;
		if (py0 < 0) py0 = 0; if (py1 > menu_sw_h) py1 = menu_sw_h;

		uint8_t sa = (uint8_t)(a * 255.0f);
		uint8_t ia = 255 - sa;
		uint8_t sb = (uint8_t)(b * 255.0f);
		uint8_t sg = (uint8_t)(g * 255.0f);
		uint8_t sr = (uint8_t)(r * 255.0f);

		for (int yy = py0; yy < py1; yy++) {
			for (int xx = px0; xx < px1; xx++) {
				uint32_t dst = menu_sw_buf[yy * menu_sw_w + xx];
				uint8_t db = (dst >> 0) & 0xFF;
				uint8_t dg = (dst >> 8) & 0xFF;
				uint8_t dr = (dst >> 16) & 0xFF;
				uint8_t rb = (uint8_t)((db * ia + sb * sa) / 255);
				uint8_t rg = (uint8_t)((dg * ia + sg * sa) / 255);
				uint8_t rr = (uint8_t)((dr * ia + sr * sa) / 255);
				menu_sw_buf[yy * menu_sw_w + xx] = rb | (rg << 8) | (rr << 16) | 0xFF000000u;
			}
		}
		return;
	}

	if (!menu_gl_inited) menu_init_gl();

	float verts[] = {
		nx0, ny0,  nx1, ny0,  nx0, ny1,
		nx1, ny0,  nx1, ny1,  nx0, ny1,
	};

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(menu_shader);
	glUniform4f(menu_u_color, r, g, b, a);

	glBindVertexArray(menu_vao);
	glBindBuffer(GL_ARRAY_BUFFER, menu_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(menu_i_pos);
	glVertexAttribPointer(menu_i_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);

	glDrawArrays(GL_TRIANGLES, 0, 6);

	glDisableVertexAttribArray(menu_i_pos);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glUseProgram(0);
}

static void render_overlay_bg(float alpha)
{
	draw_rect(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, alpha);
}

/* Render a menu item. Returns the Y offset for the next item. */
static float render_item(float x, float y, const char *text, bool selected, bool is_header)
{
	font_color color;
	float item_scale = scale;

	if (is_header) {
		color = FONT_COLOR_YELLOW;
		item_scale = scale * 1.1f;
	} else if (selected) {
		color = FONT_COLOR_WHITE;

		/* Draw subtle highlight bar behind selected item */
		float tw = font_text_width(text, item_scale);
		float th = font_text_height(item_scale);
		float pad = 4.0f;

		float nx0 = ((x - pad) / scr_w) * 2.0f - 1.0f;
		float ny0 = 1.0f - ((y + th + pad) / scr_h) * 2.0f;
		float nx1 = ((x + tw + pad) / scr_w) * 2.0f - 1.0f;
		float ny1 = 1.0f - ((y - pad) / scr_h) * 2.0f;

		draw_rect(nx0, ny0, nx1, ny1, 1.0f, 1.0f, 1.0f, 0.08f);
	} else {
		color = FONT_COLOR_GRAY;
	}

	font_render_text(x, y, text, color, item_scale, scr_w, scr_h);
	return y + font_text_height(item_scale) + 4.0f;
}

/* Render a key-value item with left/right arrows for selected */
static float render_kv_item(float x, float y, const char *key, const char *value, bool selected)
{
	char buf[256];
	if (selected)
		snprintf(buf, sizeof(buf), "< %s: %s >", key, value);
	else
		snprintf(buf, sizeof(buf), "  %s: %s", key, value);
	return render_item(x, y, buf, selected, false);
}

/* ─────────────────── Button name helper ─────────────────── */

static const char *retro_button_name(int id)
{
	switch (id) {
		case RETRO_DEVICE_ID_JOYPAD_B:      return lang_get(STR_BTN_B);
		case RETRO_DEVICE_ID_JOYPAD_Y:      return lang_get(STR_BTN_Y);
		case RETRO_DEVICE_ID_JOYPAD_SELECT: return lang_get(STR_BTN_SELECT);
		case RETRO_DEVICE_ID_JOYPAD_START:  return lang_get(STR_BTN_START);
		case RETRO_DEVICE_ID_JOYPAD_UP:     return lang_get(STR_BTN_UP);
		case RETRO_DEVICE_ID_JOYPAD_DOWN:   return lang_get(STR_BTN_DOWN);
		case RETRO_DEVICE_ID_JOYPAD_LEFT:   return lang_get(STR_BTN_LEFT);
		case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return lang_get(STR_BTN_RIGHT);
		case RETRO_DEVICE_ID_JOYPAD_A:      return lang_get(STR_BTN_A);
		case RETRO_DEVICE_ID_JOYPAD_X:      return lang_get(STR_BTN_X);
		case RETRO_DEVICE_ID_JOYPAD_L:      return lang_get(STR_BTN_L);
		case RETRO_DEVICE_ID_JOYPAD_R:      return lang_get(STR_BTN_R);
		case RETRO_DEVICE_ID_JOYPAD_L2:     return lang_get(STR_BTN_L2);
		case RETRO_DEVICE_ID_JOYPAD_R2:     return lang_get(STR_BTN_R2);
		case RETRO_DEVICE_ID_JOYPAD_L3:     return lang_get(STR_BTN_L3);
		case RETRO_DEVICE_ID_JOYPAD_R3:     return lang_get(STR_BTN_R3);
		default: return "???";
	}
}

static bool str_contains_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !needle[0])
		return false;

	for (; *haystack; haystack++) {
		const char *h = haystack;
		const char *n = needle;

		while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
			h++;
			n++;
		}

		if (*n == '\0')
			return true;
	}

	return false;
}

static bool controller_uses_xbox_labels(int port)
{
	const char *name = NULL;

	if (port < 0 || !glfwJoystickIsGamepad(port))
		return false;

	name = glfwGetGamepadName(port);
	if (!name)
		return false;

	return str_contains_ci(name, "xbox") ||
	       str_contains_ci(name, "microsoft");
}

static const char *retro_button_display_name(int port, int id)
{
	if (controller_uses_xbox_labels(port)) {
		switch (id) {
			case RETRO_DEVICE_ID_JOYPAD_B: return lang_get(STR_BTN_A);
			case RETRO_DEVICE_ID_JOYPAD_Y: return lang_get(STR_BTN_X);
			case RETRO_DEVICE_ID_JOYPAD_A: return lang_get(STR_BTN_B);
			case RETRO_DEVICE_ID_JOYPAD_X: return lang_get(STR_BTN_Y);
			default: break;
		}
	}

	return retro_button_name(id);
}

static const char *glfw_button_name(int btn)
{
	switch (btn) {
		case GLFW_GAMEPAD_BUTTON_A:            return "A";
		case GLFW_GAMEPAD_BUTTON_B:            return "B";
		case GLFW_GAMEPAD_BUTTON_X:            return "X";
		case GLFW_GAMEPAD_BUTTON_Y:            return "Y";
		case GLFW_GAMEPAD_BUTTON_LEFT_BUMPER:  return "LB";
		case GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER: return "RB";
		case GLFW_GAMEPAD_BUTTON_BACK:         return "Back";
		case GLFW_GAMEPAD_BUTTON_START:        return "Start";
		case GLFW_GAMEPAD_BUTTON_GUIDE:        return "Guide";
		case GLFW_GAMEPAD_BUTTON_LEFT_THUMB:   return "L3";
		case GLFW_GAMEPAD_BUTTON_RIGHT_THUMB:  return "R3";
		case GLFW_GAMEPAD_BUTTON_DPAD_UP:      return "D-Up";
		case GLFW_GAMEPAD_BUTTON_DPAD_RIGHT:   return "D-Right";
		case GLFW_GAMEPAD_BUTTON_DPAD_DOWN:    return "D-Down";
		case GLFW_GAMEPAD_BUTTON_DPAD_LEFT:    return "D-Left";
		default: return "???";
	}
}

/* ─────────────────── Page Renderers ─────────────────── */

/* Main Menu */
#define MAIN_ITEMS 7

static void render_page_main(void)
{
	float x = scr_w * 0.1f;
	float y = scr_h * 0.15f;

	y = render_item(x, y, lang_get(STR_SETTINGS), false, true);
	y += 10.0f;

	const char *items[MAIN_ITEMS] = {
		lang_get(STR_RESUME),
		lang_get(STR_VIDEO_SETTINGS),
		lang_get(STR_AUDIO_SETTINGS),
		lang_get(STR_INPUT_REMAP),
		lang_get(STR_CORE_OPTIONS),
		lang_get(STR_LANGUAGE),
		lang_get(STR_EXIT_GAME),
	};

	for (int i = 0; i < MAIN_ITEMS; i++) {
		if (i == 5)
			y = render_kv_item(x, y, items[i], lang_display_name(lang_current()), i == menu_sel);
		else
			y = render_item(x, y, items[i], i == menu_sel, false);
	}

	/* Footer hint */
	float fy = scr_h * 0.9f;
	font_render_text(x, fy, lang_get(STR_HINT_MAIN),
	                 FONT_COLOR_GRAY, scale * 0.7f, scr_w, scr_h);
}

static bool input_page_main(void)
{
	if (nav_up() && input_debounce()) {
		menu_sel--;
		if (menu_sel < 0) menu_sel = MAIN_ITEMS - 1;
	}
	if (nav_down() && input_debounce()) {
		menu_sel++;
		if (menu_sel >= MAIN_ITEMS) menu_sel = 0;
	}

	/* Language: cycle with left/right */
	if (menu_sel == 5 && (nav_left() || nav_right()) && input_debounce()) {
		int dir = nav_right() ? 1 : -1;
		lang_cycle(dir);
	}

	if (nav_confirm()) {
		switch (menu_sel) {
			case 0: menu_toggle(); break; /* Resume */
			case 1: menu_page = MENU_PAGE_VIDEO; menu_sel = 0; break;
			case 2: menu_page = MENU_PAGE_AUDIO; menu_sel = 0; break;
			case 3: menu_page = MENU_PAGE_INPUT; menu_sel = 0; break;
			case 4: menu_page = MENU_PAGE_CORE_OPTIONS; menu_sel = 0; menu_scroll = 0; break;
			case 5: lang_cycle(1); break; /* Language: cycle on confirm */
			case 6: menu_page = MENU_PAGE_CONFIRM_EXIT; menu_sel = 0; break;
		}
	}
	if (nav_back()) {
		menu_toggle(); /* Close menu */
	}
	return false;
}

/* Video Settings */
static void render_page_video(void)
{
	float x = scr_w * 0.1f;
	float y = scr_h * 0.15f;

	y = render_item(x, y, lang_get(STR_VIDEO_HEADER), false, true);
	y += 10.0f;

	const char *fs_val = video_is_fullscreen() ? lang_get(STR_ON) : lang_get(STR_OFF);
	y = render_kv_item(x, y, lang_get(STR_FULLSCREEN), fs_val, menu_sel == 0);

	y = render_kv_item(x, y, lang_get(STR_SHADER), g_cfg.shader, menu_sel == 1);

	y = render_kv_item(x, y, lang_get(STR_FILTER), g_cfg.filter, menu_sel == 2);

	y = render_item(x, y, lang_get(STR_ASPECT_RATIO), menu_sel == 3, false);

	y += 10.0f;
	y = render_item(x, y, lang_get(STR_BACK), menu_sel == 4, false);

	float fy = scr_h * 0.9f;
	font_render_text(x, fy, lang_get(STR_HINT_VIDEO),
	                 FONT_COLOR_GRAY, scale * 0.7f, scr_w, scr_h);
}

static bool input_page_video(void)
{
	if (nav_up() && input_debounce()) {
		menu_sel--;
		if (menu_sel < 0) menu_sel = 4;
	}
	if (nav_down() && input_debounce()) {
		menu_sel++;
		if (menu_sel > 4) menu_sel = 0;
	}

	if (nav_confirm()) {
		switch (menu_sel) {
			case 0: /* Fullscreen toggle */
				video_toggle_fullscreen();
				break;
			case 1: /* Shader cycle */
				if (strcmp(g_cfg.shader, "default") == 0)
					g_cfg.shader = "zfast-crt";
				else if (strcmp(g_cfg.shader, "zfast-crt") == 0)
					g_cfg.shader = "zfast-lcd";
				else
					g_cfg.shader = "default";
				video_reload_shader();
				break;
			case 2: /* Filter cycle */
				if (strcmp(g_cfg.filter, "nearest") == 0)
					g_cfg.filter = "linear";
				else
					g_cfg.filter = "nearest";
				video_reload_filter();
				break;
			case 3: /* Aspect Ratio */
				menu_page = MENU_PAGE_ASPECT;
				menu_sel = 0;
				break;
			case 4: /* Back */
				menu_page = MENU_PAGE_MAIN;
				menu_sel = 1;
				break;
		}
	} else if ((nav_left() || nav_right()) && input_debounce()) {
		switch (menu_sel) {
			case 1: /* Shader cycle fallback for left/right */
				if (strcmp(g_cfg.shader, "default") == 0) g_cfg.shader = "zfast-crt";
				else if (strcmp(g_cfg.shader, "zfast-crt") == 0) g_cfg.shader = "zfast-lcd";
				else g_cfg.shader = "default";
				video_reload_shader();
				break;
			case 2: /* Filter cycle fallback for left/right */
				if (strcmp(g_cfg.filter, "nearest") == 0) g_cfg.filter = "linear";
				else g_cfg.filter = "nearest";
				video_reload_filter();
				break;
		}
	}

	if (nav_back()) {
		menu_page = MENU_PAGE_MAIN;
		menu_sel = 1;
	}
	return false;
}

/* Audio Settings */
static void render_page_audio(void)
{
	float x = scr_w * 0.1f;
	float y = scr_h * 0.15f;

	y = render_item(x, y, lang_get(STR_AUDIO_HEADER), false, true);
	y += 10.0f;

	char vol[16];
	snprintf(vol, sizeof(vol), "%d%%", audio_get_volume());
	y = render_kv_item(x, y, lang_get(STR_VOLUME), vol, menu_sel == 0);

	const char *mute_val = audio_get_mute() ? lang_get(STR_ON) : lang_get(STR_OFF);
	y = render_kv_item(x, y, lang_get(STR_MUTE), mute_val, menu_sel == 1);

	y += 10.0f;
	y = render_item(x, y, lang_get(STR_BACK), menu_sel == 2, false);

	float fy = scr_h * 0.9f;
	font_render_text(x, fy, lang_get(STR_HINT_AUDIO), FONT_COLOR_GRAY, scale * 0.7f, scr_w, scr_h);
}

static bool input_page_audio(void)
{
	if (nav_up() && input_debounce()) {
		menu_sel--;
		if (menu_sel < 0) menu_sel = 2;
	}
	if (nav_down() && input_debounce()) {
		menu_sel++;
		if (menu_sel > 2) menu_sel = 0;
	}

	if (nav_confirm()) {
		switch (menu_sel) {
			case 1: /* Mute toggle */
				audio_set_mute(!audio_get_mute());
				break;
			case 2: /* Back */
				menu_page = MENU_PAGE_MAIN;
				menu_sel = 2;
				break;
		}
	} else if (nav_left() && menu_sel == 0) {
		int vol = audio_get_volume() - 10;
		if (vol < 0) vol = 0;
		audio_set_volume(vol);
	} else if (nav_right() && menu_sel == 0) {
		int vol = audio_get_volume() + 10;
		if (vol > 100) vol = 100;
		audio_set_volume(vol);
	}

	if (nav_back()) {
		menu_page = MENU_PAGE_MAIN;
		menu_sel = 2;
	}
	return false;
}

/* Input / Remap page */
#define REMAP_BUTTONS 14
#define MAX_REMAP_PORTS 4

static int remap_port_sel = 0; /* Which port we're configuring */

static void render_page_input(void)
{
	float x = scr_w * 0.1f;
	float y = scr_h * 0.1f;

	y = render_item(x, y, lang_get(STR_INPUT_HEADER), false, true);
	y += 10.0f;

	/* Port selector */
	char port_buf[64];
	snprintf(port_buf, sizeof(port_buf), "< %s: %d >", lang_get(STR_CONTROLLER_PORT), remap_port_sel + 1);
	y = render_item(x, y, port_buf, menu_sel == 0, false);
	y += 6.0f;

	/* Button mappings for current port */
	for (int i = 0; i < REMAP_BUTTONS; i++) {
		int sel_idx = i + 1;
		const char *btn_name = retro_button_display_name(remap_port_sel, i);
		unsigned mapped = remap_get(remap_port_sel, i);

		char buf[128];
		if (remap_capturing && remap_capture_button == i && remap_capture_port == remap_port_sel) {
			snprintf(buf, sizeof(buf), "  %s: %s", btn_name, lang_get(STR_PRESS_BUTTON));
			y = render_item(x, y, buf, true, false);
		} else {
			snprintf(buf, sizeof(buf), "%s -> %s", btn_name, glfw_button_name(mapped));
			y = render_kv_item(x, y, "", buf, menu_sel == sel_idx);
		}
	}

	y += 6.0f;
	y = render_item(x, y, lang_get(STR_RESET_DEFAULTS), menu_sel == REMAP_BUTTONS + 1, false);
	y = render_item(x, y, lang_get(STR_BACK), menu_sel == REMAP_BUTTONS + 2, false);

	float fy = scr_h * 0.92f;
	font_render_text(x, fy, lang_get(STR_HINT_INPUT),
	                 FONT_COLOR_GRAY, scale * 0.65f, scr_w, scr_h);
}

static bool input_page_input(void)
{
	int total_items = REMAP_BUTTONS + 3; /* port selector + 14 buttons + reset + back */

	/* Remap capture mode */
	if (remap_capturing) {
		/* Also check keyboard Escape to cancel */
		if (key_pressed(GLFW_KEY_ESCAPE)) {
			remap_capturing = false;
			remap_wait_release = false;
			last_input_time = glfwGetTime() + 0.3;
			return false;
		}

		/* Phase 1: Wait for ALL buttons to be released first */
		if (remap_wait_release) {
			bool any_pressed = false;
			/* Check keyboard confirm keys */
			if (key_pressed(GLFW_KEY_ENTER)) any_pressed = true;
			/* Check all gamepad buttons */
			for (int port = 0; port < MAX_REMAP_PORTS && !any_pressed; port++) {
				if (!glfwJoystickIsGamepad(port)) continue;
				GLFWgamepadstate pad;
				if (!glfwGetGamepadState(port, &pad)) continue;
				for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++) {
					if (pad.buttons[b] == GLFW_PRESS) {
						any_pressed = true;
						break;
					}
				}
			}
			if (!any_pressed)
				remap_wait_release = false; /* All released, move to phase 2 */
			return false;
		}

		/* Phase 2: Capture the first NEW button press */
		for (int port = 0; port < MAX_REMAP_PORTS; port++) {
			if (!glfwJoystickIsGamepad(port)) continue;
			GLFWgamepadstate pad;
			if (!glfwGetGamepadState(port, &pad)) continue;
			for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; b++) {
				if (pad.buttons[b] == GLFW_PRESS) {
					remap_set(remap_capture_port, remap_capture_button, b);
					remap_capturing = false;
					remap_save();
					last_input_time = glfwGetTime() + 0.3; /* Extra debounce */
					return false;
				}
			}
		}
		return false;
	}

	if (nav_up() && input_debounce()) {
		menu_sel--;
		if (menu_sel < 0) menu_sel = total_items - 1;
	}
	if (nav_down() && input_debounce()) {
		menu_sel++;
		if (menu_sel >= total_items) menu_sel = 0;
	}

	/* Port switching with left/right on port selector, or LB/RB anywhere */
	if (menu_sel == 0 && (nav_left() || nav_right()) && input_debounce()) {
		if (nav_right()) remap_port_sel = (remap_port_sel + 1) % MAX_REMAP_PORTS;
		else remap_port_sel = (remap_port_sel + MAX_REMAP_PORTS - 1) % MAX_REMAP_PORTS;
	}
	if ((pad_button(0, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER) || key_pressed(GLFW_KEY_PAGE_UP)) && input_debounce()) {
		remap_port_sel = (remap_port_sel + MAX_REMAP_PORTS - 1) % MAX_REMAP_PORTS;
	}
	if ((pad_button(0, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER) || key_pressed(GLFW_KEY_PAGE_DOWN)) && input_debounce()) {
		remap_port_sel = (remap_port_sel + 1) % MAX_REMAP_PORTS;
	}

	if (nav_confirm() && input_debounce()) {
		if (menu_sel == 0) {
			/* Port selector — do nothing on confirm, use left/right */
		} else if (menu_sel >= 1 && menu_sel <= REMAP_BUTTONS) {
			/* Start capture for this button — wait for release first */
			remap_capturing = true;
			remap_wait_release = true;
			remap_capture_port = remap_port_sel;
			remap_capture_button = menu_sel - 1;
		} else if (menu_sel == REMAP_BUTTONS + 1) {
			/* Reset defaults for current port */
			remap_reset_defaults(remap_port_sel);
			remap_save();
		} else if (menu_sel == REMAP_BUTTONS + 2) {
			/* Back */
			menu_page = MENU_PAGE_MAIN;
			menu_sel = 2;
		}
	}
	if (nav_back() && input_debounce()) {
		menu_page = MENU_PAGE_MAIN;
		menu_sel = 2;
	}
	return false;
}

/* Core Options page */
static void render_page_core_options(void)
{
	float x = scr_w * 0.1f;
	float y = scr_h * 0.1f;

	y = render_item(x, y, lang_get(STR_CORE_OPT_HEADER), false, true);
	y += 10.0f;

	int count = opt_count();
	if (count == 0) {
		y = render_item(x, y, lang_get(STR_NO_CORE_OPTIONS), false, false);
	} else {
		int visible_items = (int)((scr_h * 0.7f) / (font_text_height(scale) + 4.0f));
		if (visible_items < 1) visible_items = 1;

		if (menu_sel < menu_scroll) menu_scroll = menu_sel;
		if (menu_sel >= menu_scroll + visible_items) menu_scroll = menu_sel - visible_items + 1;

		int end = menu_scroll + visible_items;
		if (end > count + 1) end = count + 1;

		for (int i = menu_scroll; i < end; i++) {
			if (i < count) {
				const core_option_t *entry = opt_get_entry(i);
				const char *key = entry->key;
				const char *val = entry->labels[entry->selected];
				const char *desc = entry->desc;
				const char *display = desc[0] ? desc : key;
				y = render_kv_item(x, y, display, val[0] ? val : "?", menu_sel == i);
			} else {
				y += 6.0f;
				y = render_item(x, y, lang_get(STR_BACK), menu_sel == count, false);
			}
		}
	}

	float fy = scr_h * 0.92f;
	font_render_text(x, fy, lang_get(STR_HINT_CORE_OPTIONS),
	                 FONT_COLOR_GRAY, scale * 0.7f, scr_w, scr_h);
}

static bool input_page_core_options(void)
{
	int count = opt_count();
	int total = count + 1; /* options + back */

	if (nav_up() && input_debounce()) {
		menu_sel--;
		if (menu_sel < 0) menu_sel = total - 1;
	}
	if (nav_down() && input_debounce()) {
		menu_sel++;
		if (menu_sel >= total) menu_sel = 0;
	}

	if ((nav_left() || nav_right()) && input_debounce() && menu_sel < count) {
		int dir = nav_right() ? 1 : -1;
		opt_cycle(menu_sel, dir);
	}

	if (nav_confirm() && input_debounce()) {
		if (menu_sel < count) {
			opt_cycle(menu_sel, 1);
		} else {
			/* Back */
			menu_page = MENU_PAGE_MAIN;
			menu_sel = 3;
			opt_save("./options.ini");
		}
	}
	if (nav_back() && input_debounce()) {
		menu_page = MENU_PAGE_MAIN;
		menu_sel = 3;
		opt_save("./options.ini");
	}
	return false;
}

/* Confirm Exit page */
static void render_page_confirm_exit(void)
{
	float x = scr_w * 0.3f;
	float y = scr_h * 0.35f;

	y = render_item(x, y, lang_get(STR_EXIT_CONFIRM), false, true);
	y += 20.0f;
	y = render_item(x, y, lang_get(STR_YES_QUIT), menu_sel == 0, false);
	y = render_item(x, y, lang_get(STR_NO_CANCEL), menu_sel == 1, false);
}

static bool input_page_confirm_exit(void)
{
	if (nav_up() && input_debounce()) menu_sel = menu_sel ? 0 : 1;
	if (nav_down() && input_debounce()) menu_sel = menu_sel ? 0 : 1;

	if (nav_confirm() && input_debounce()) {
		if (menu_sel == 0) return true; /* QUIT */
		menu_page = MENU_PAGE_MAIN;
		menu_sel = 6;
	}
	if (nav_back() && input_debounce()) {
		menu_page = MENU_PAGE_MAIN;
		menu_sel = 6;
	}
	return false;
}

/* ─────────────────── Aspect Ratio Page ─────────────────── */

#define ASPECT_PAGE_ITEMS 4  /* Mode, Custom Edit, Reset, Back */

static void render_page_aspect(void)
{
	float x = scr_w * 0.1f;
	float y = scr_h * 0.15f;

	y = render_item(x, y, lang_get(STR_ASPECT_HEADER), false, true);
	y += 10.0f;

	/* Mode selector */
	char mode_buf[128];
	snprintf(mode_buf, sizeof(mode_buf), "%s: < %s >",
	         lang_get(STR_ASPECT_RATIO), aspect_mode_name(aspect_get_mode()));
	y = render_item(x, y, mode_buf, menu_sel == 0, false);

	y += 6.0f;
	y = render_item(x, y, lang_get(STR_ASPECT_CUSTOM_EDIT), menu_sel == 1, false);
	y = render_item(x, y, lang_get(STR_ASPECT_RESET), menu_sel == 2, false);
	y += 6.0f;
	y = render_item(x, y, lang_get(STR_BACK), menu_sel == 3, false);

	/* Show current custom values if in custom mode */
	if (aspect_get_mode() == ASPECT_CUSTOM) {
		aspect_custom_t *c = aspect_get_custom();
		y += 10.0f;
		char info[128];
		snprintf(info, sizeof(info), "X:%+d  Y:%+d  W:%+d  H:%+d",
		         c->off_x, c->off_y, c->adj_w, c->adj_h);
		font_render_text(x + 20.0f, y, info,
		                 FONT_COLOR_YELLOW, scale * 0.8f, scr_w, scr_h);
	}

	float fy = scr_h * 0.9f;
	font_render_text(x, fy, lang_get(STR_HINT_ASPECT),
	                 FONT_COLOR_GRAY, scale * 0.7f, scr_w, scr_h);
}

static bool input_page_aspect(void)
{
	if (nav_up() && input_debounce()) {
		menu_sel--;
		if (menu_sel < 0) menu_sel = ASPECT_PAGE_ITEMS - 1;
	}
	if (nav_down() && input_debounce()) {
		menu_sel++;
		if (menu_sel >= ASPECT_PAGE_ITEMS) menu_sel = 0;
	}

	/* Cycle mode with left/right on item 0 */
	if (menu_sel == 0 && (nav_left() || nav_right()) && input_debounce()) {
		aspect_cycle(nav_right() ? 1 : -1);
		aspect_save();
	}

	if (nav_confirm() && input_debounce()) {
		switch (menu_sel) {
			case 0: aspect_cycle(1); aspect_save(); break;
			case 1:
				aspect_set_mode(ASPECT_CUSTOM);
				menu_page = MENU_PAGE_ASPECT_EDIT;
				break;
			case 2:
				aspect_custom_reset();
				aspect_save();
				break;
			case 3:
				menu_page = MENU_PAGE_MAIN;
				menu_sel = 2;
				break;
		}
	}
	if (nav_back() && input_debounce()) {
		menu_page = MENU_PAGE_MAIN;
		menu_sel = 2;
	}
	return false;
}

/* ─────────────────── Aspect Custom Edit Page ─────────────────── */

#define ASPECT_EDIT_STEP 2

static void render_page_aspect_edit(void)
{
	float x = scr_w * 0.1f;
	float y = scr_h * 0.15f;

	y = render_item(x, y, lang_get(STR_ASPECT_CUSTOM_EDIT), false, true);
	y += 10.0f;

	aspect_custom_t *c = aspect_get_custom();
	char buf[128];

	snprintf(buf, sizeof(buf), "X: %+d", c->off_x);
	y = render_item(x, y, buf, false, false);
	snprintf(buf, sizeof(buf), "Y: %+d", c->off_y);
	y = render_item(x, y, buf, false, false);
	snprintf(buf, sizeof(buf), "W: %+d", c->adj_w);
	y = render_item(x, y, buf, false, false);
	snprintf(buf, sizeof(buf), "H: %+d", c->adj_h);
	y = render_item(x, y, buf, false, false);

	float fy = scr_h * 0.9f;
	font_render_text(x, fy, lang_get(STR_HINT_ASPECT_EDIT),
	                 FONT_COLOR_GRAY, scale * 0.65f, scr_w, scr_h);
}

static bool input_page_aspect_edit(void)
{
	aspect_custom_t *c = aspect_get_custom();

	/* D-Pad: move position */
	if (nav_up() && input_debounce())    c->off_y -= ASPECT_EDIT_STEP;
	if (nav_down() && input_debounce())  c->off_y += ASPECT_EDIT_STEP;
	if (nav_left() && input_debounce())  c->off_x -= ASPECT_EDIT_STEP;
	if (nav_right() && input_debounce()) c->off_x += ASPECT_EDIT_STEP;

	/* LB/RB: width */
	if (nav_lb() && input_debounce()) c->adj_w -= ASPECT_EDIT_STEP;
	if (nav_rb() && input_debounce()) c->adj_w += ASPECT_EDIT_STEP;

	/* LT/RT: height */
	if (nav_lt() && input_debounce()) c->adj_h -= ASPECT_EDIT_STEP;
	if (nav_rt() && input_debounce()) c->adj_h += ASPECT_EDIT_STEP;

	/* Confirm: save and go back */
	if (nav_confirm() && input_debounce()) {
		aspect_save();
		menu_page = MENU_PAGE_ASPECT;
		menu_sel = 1;
	}
	/* Cancel: go back without saving (values already modified in-place, but user expects cancel) */
	if (nav_back() && input_debounce()) {
		menu_page = MENU_PAGE_ASPECT;
		menu_sel = 1;
	}
	return false;
}

/* ─────────────────── Public API ─────────────────── */

void menu_init(void)
{
	menu_active = false;
	menu_page = MENU_PAGE_MAIN;
	menu_sel = 0;
	remap_capturing = false;
}

void menu_deinit(void)
{
	if (menu_shader) { glDeleteProgram(menu_shader); menu_shader = 0; }
	if (menu_vao) { glDeleteVertexArrays(1, &menu_vao); menu_vao = 0; }
	if (menu_vbo) { glDeleteBuffers(1, &menu_vbo); menu_vbo = 0; }
	menu_gl_inited = false;
}

void menu_toggle(void)
{
	menu_active = !menu_active;
	log_printf("menu", "menu_toggle active=%d page=%d sel=%d", menu_active ? 1 : 0, (int)menu_page, menu_sel);
	if (menu_active) {
		menu_page = MENU_PAGE_MAIN;
		menu_sel = 0;
		remap_capturing = false;
		prev_confirm = key_pressed(GLFW_KEY_ENTER) || pad_button(0, GLFW_GAMEPAD_BUTTON_A);
		prev_back = key_pressed(GLFW_KEY_ESCAPE) || pad_button(0, GLFW_GAMEPAD_BUTTON_B);
		last_input_time = glfwGetTime() + 0.25; /* Initial debounce */
	}
}

bool menu_is_active(void)
{
	return menu_active;
}

bool menu_input(void)
{
	switch (menu_page) {
		case MENU_PAGE_MAIN:         return input_page_main();
		case MENU_PAGE_VIDEO:        return input_page_video();
		case MENU_PAGE_AUDIO:        return input_page_audio();
		case MENU_PAGE_INPUT:        return input_page_input();
		case MENU_PAGE_CORE_OPTIONS: return input_page_core_options();
		case MENU_PAGE_ASPECT:       return input_page_aspect();
		case MENU_PAGE_ASPECT_EDIT:  return input_page_aspect_edit();
		case MENU_PAGE_CONFIRM_EXIT: return input_page_confirm_exit();
		default: break;
	}
	return false;
}

void menu_render(void)
{
	if (menu_sw_buf) {
		/* Software mode: use the pre-set dimensions */
		scr_w = menu_sw_w;
		scr_h = menu_sw_h;
	} else {
		/* GL mode: query framebuffer */
		glfwGetFramebufferSize(window, &scr_w, &scr_h);
	}
	if (scr_w <= 0 || scr_h <= 0) return;

	/* Calculate text scale based on screen height */
	scale = (float)scr_h / 480.0f * 1.5f;
	if (scale < 1.0f) scale = 1.0f;
	if (scale > 3.0f) scale = 3.0f;

	if (!menu_sw_buf)
		glViewport(0, 0, scr_w, scr_h);

	float bg_alpha = 0.78f;
	if (menu_page == MENU_PAGE_VIDEO && (menu_sel == 1 || menu_sel == 2)) {
		bg_alpha = 0.0f; /* 100% transparent so user can preview shaders */
	} else if (menu_page == MENU_PAGE_ASPECT || menu_page == MENU_PAGE_ASPECT_EDIT) {
		bg_alpha = 0.0f;
	}

	/* Draw semi-transparent background */
	render_overlay_bg(bg_alpha);

	/* Draw current page */
	switch (menu_page) {
		case MENU_PAGE_MAIN:         render_page_main(); break;
		case MENU_PAGE_VIDEO:        render_page_video(); break;
		case MENU_PAGE_AUDIO:        render_page_audio(); break;
		case MENU_PAGE_INPUT:        render_page_input(); break;
		case MENU_PAGE_CORE_OPTIONS: render_page_core_options(); break;
		case MENU_PAGE_ASPECT:       render_page_aspect(); break;
		case MENU_PAGE_ASPECT_EDIT:  render_page_aspect_edit(); break;
		case MENU_PAGE_CONFIRM_EXIT: render_page_confirm_exit(); break;
		default: break;
	}

	if (!menu_sw_buf)
		glDisable(GL_BLEND);
}
