#ifndef FONT_H
#define FONT_H

#include <stdbool.h>
#include <stdint.h>

/* Bitmap font renderer using OpenGL textured quads.
 * Uses an embedded 8x16 CP437-style monospace bitmap font for Latin text and
 * a Windows Unicode fallback for scripts outside the built-in atlas.
 * No external font files are required. */

typedef struct {
	float r, g, b, a;
} font_color;

#define FONT_COLOR_WHITE  (font_color){1.0f, 1.0f, 1.0f, 1.0f}
#define FONT_COLOR_GRAY   (font_color){0.376f, 0.376f, 0.376f, 1.0f}
#define FONT_COLOR_YELLOW (font_color){1.0f, 0.85f, 0.0f, 1.0f}
#define FONT_COLOR_RED    (font_color){1.0f, 0.2f, 0.2f, 1.0f}
#define FONT_COLOR_GREEN  (font_color){0.2f, 1.0f, 0.4f, 1.0f}

/* Initialize the font system. Call once after OpenGL context is ready. */
void font_init(void);

/* Software rendering target (for Vulkan menu overlay).
 * When set, font_render_text draws to a BGRA CPU buffer instead of GL.
 * Pass NULL buf to clear. */
void font_set_sw_target(uint32_t *buf, int w, int h);
void font_clear_sw_target(void);

/* Clean up font resources. */
void font_deinit(void);

/* Render a string at pixel coordinates (top-left origin).
 * scale: multiplier for the 8x16 base glyph size.
 * screen_w, screen_h: current framebuffer dimensions. */
void font_render_text(float x, float y, const char *text,
                      font_color color, float scale,
                      int screen_w, int screen_h);

/* Returns the width in pixels of a string at given scale. */
float font_text_width(const char *text, float scale);

/* Returns the height in pixels of a line at given scale. */
float font_text_height(float scale);

#endif /* FONT_H */
