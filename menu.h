#ifndef MENU_H
#define MENU_H

#include <stdbool.h>
#include <stdint.h>

/* Menu pages */
typedef enum {
	MENU_PAGE_MAIN = 0,
	MENU_PAGE_VIDEO,
	MENU_PAGE_AUDIO,
	MENU_PAGE_INPUT,
	MENU_PAGE_REMAP,
	MENU_PAGE_CORE_OPTIONS,
	MENU_PAGE_ASPECT,
	MENU_PAGE_ASPECT_EDIT,
	MENU_PAGE_CONFIRM_EXIT,
} menu_page_t;

/* Initialize the overlay menu system. Call after OpenGL context + font_init(). */
void menu_init(void);

/* Clean up menu resources. */
void menu_deinit(void);

/* Toggle menu visibility. */
void menu_toggle(void);

/* Returns true if the menu overlay is currently active. */
bool menu_is_active(void);

/* Process input for the menu (call instead of game input when menu is active).
 * Returns true if the app should quit. */
bool menu_input(void);

/* Render the menu overlay on top of the current frame. */
void menu_render(void);

/* Software rendering target (for Vulkan menu overlay).
 * When set, menu draws to a BGRA CPU buffer instead of GL. */
void menu_set_sw_target(uint32_t *buf, int w, int h);
void menu_clear_sw_target(void);

#endif /* MENU_H */
