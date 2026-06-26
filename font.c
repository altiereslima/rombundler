/*
 * font.c — TrueType font renderer using stb_truetype for ROMBundler.
 *
 * Renders text using the embedded Cabin-Bold font, baked into a texture atlas at runtime.
 * Maintains Windows GDI Unicode fallback for characters outside Latin-1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "font.h"
#include "lang.h"
#include "cabin_font_data.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define FONT_CHAR_W 10
#define FONT_CHAR_H 22

/* Texture atlas size */
#define ATLAS_W 1024
#define ATLAS_H 512

/* Baked character data for codepoints 32 to 255 (224 characters) */
static stbtt_bakedchar chardata[224];
static unsigned char *temp_bitmap = NULL;

static GLuint font_tex = 0;
static GLuint font_shader = 0;
static GLuint font_vao = 0;
static GLuint font_vbo = 0;
static GLint font_u_tex = -1;
static GLint font_u_color = -1;
static GLint font_i_pos = -1;
static GLint font_i_coord = -1;

#ifdef _WIN32
static char *font_strdup(const char *s)
{
	size_t len = strlen(s);
	char *d = (char *)malloc(len + 1);
	if (d) {
		strcpy(d, s);
	}
	return d;
}

typedef struct {
	char *text;
	float scale;
	int width;
	int height;
	GLuint gl_tex;
	unsigned char *alpha;
	uint32_t last_used;
} unicode_cache_entry_t;

#define UNICODE_CACHE_MAX 128
static unicode_cache_entry_t unicode_cache[UNICODE_CACHE_MAX];
static int unicode_cache_count = 0;
static uint32_t unicode_cache_timer = 0;

void font_clear_unicode_cache(void)
{
	for (int i = 0; i < unicode_cache_count; i++) {
		if (unicode_cache[i].text) {
			free(unicode_cache[i].text);
			unicode_cache[i].text = NULL;
		}
		if (unicode_cache[i].gl_tex) {
			glDeleteTextures(1, &unicode_cache[i].gl_tex);
			unicode_cache[i].gl_tex = 0;
		}
		if (unicode_cache[i].alpha) {
			free(unicode_cache[i].alpha);
			unicode_cache[i].alpha = NULL;
		}
	}
	memset(unicode_cache, 0, sizeof(unicode_cache));
	unicode_cache_count = 0;
	unicode_cache_timer = 0;
}

static unicode_cache_entry_t *find_unicode_cache_entry(const char *text, float scale)
{
	for (int i = 0; i < unicode_cache_count; i++) {
		if (unicode_cache[i].scale == scale && strcmp(unicode_cache[i].text, text) == 0) {
			unicode_cache[i].last_used = ++unicode_cache_timer;
			return &unicode_cache[i];
		}
	}
	return NULL;
}

static unicode_cache_entry_t *insert_unicode_cache_entry(const char *text, float scale, int width, int height, GLuint gl_tex, unsigned char *alpha)
{
	unicode_cache_timer++;

	unicode_cache_entry_t *existing = find_unicode_cache_entry(text, scale);
	if (existing) {
		existing->width = width;
		existing->height = height;
		if (gl_tex) {
			if (existing->gl_tex) glDeleteTextures(1, &existing->gl_tex);
			existing->gl_tex = gl_tex;
		}
		if (alpha) {
			if (existing->alpha) free(existing->alpha);
			existing->alpha = alpha;
		}
		return existing;
	}

	if (unicode_cache_count < UNICODE_CACHE_MAX) {
		int idx = unicode_cache_count++;
		unicode_cache[idx].text = font_strdup(text);
		unicode_cache[idx].scale = scale;
		unicode_cache[idx].width = width;
		unicode_cache[idx].height = height;
		unicode_cache[idx].gl_tex = gl_tex;
		unicode_cache[idx].alpha = alpha;
		unicode_cache[idx].last_used = unicode_cache_timer;
		return &unicode_cache[idx];
	}

	int lru_idx = 0;
	uint32_t min_time = unicode_cache[0].last_used;
	for (int i = 1; i < unicode_cache_count; i++) {
		if (unicode_cache[i].last_used < min_time) {
			min_time = unicode_cache[i].last_used;
			lru_idx = i;
		}
	}

	if (unicode_cache[lru_idx].text) {
		free(unicode_cache[lru_idx].text);
	}
	if (unicode_cache[lru_idx].gl_tex) {
		glDeleteTextures(1, &unicode_cache[lru_idx].gl_tex);
	}
	if (unicode_cache[lru_idx].alpha) {
		free(unicode_cache[lru_idx].alpha);
	}

	unicode_cache[lru_idx].text = font_strdup(text);
	unicode_cache[lru_idx].scale = scale;
	unicode_cache[lru_idx].width = width;
	unicode_cache[lru_idx].height = height;
	unicode_cache[lru_idx].gl_tex = gl_tex;
	unicode_cache[lru_idx].alpha = alpha;
	unicode_cache[lru_idx].last_used = unicode_cache_timer;

	return &unicode_cache[lru_idx];
}
#endif

static const char *font_vsrc =
	"attribute vec4 i_pos;\n"
	"attribute vec2 i_coord;\n"
	"varying vec2 v_coord;\n"
	"void main() {\n"
	"  v_coord = i_coord;\n"
	"  gl_Position = i_pos;\n"
	"}\n";

static const char *font_fsrc =
	"varying vec2 v_coord;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec4 u_color;\n"
	"void main() {\n"
	"  float a = texture2D(u_tex, v_coord).r;\n"
	"  gl_FragColor = vec4(u_color.rgb, u_color.a * a);\n"
	"}\n";

static int utf8_next_codepoint(const unsigned char **ptr)
{
	const unsigned char *p = *ptr;
	int c = *p++;

	if (c < 0x80) {
		*ptr = p;
		return c;
	}

	if ((c & 0xE0) == 0xC0 && p[0] && (p[0] & 0xC0) == 0x80) {
		int codepoint = ((c & 0x1F) << 6) | (p[0] & 0x3F);
		*ptr = p + 1;
		return codepoint;
	}

	if ((c & 0xF0) == 0xE0 &&
	    p[0] && p[1] &&
	    (p[0] & 0xC0) == 0x80 &&
	    (p[1] & 0xC0) == 0x80) {
		int codepoint = ((c & 0x0F) << 12) |
		                ((p[0] & 0x3F) << 6) |
		                (p[1] & 0x3F);
		*ptr = p + 2;
		return codepoint;
	}

	if ((c & 0xF8) == 0xF0 &&
	    p[0] && p[1] && p[2] &&
	    (p[0] & 0xC0) == 0x80 &&
	    (p[1] & 0xC0) == 0x80 &&
	    (p[2] & 0xC0) == 0x80) {
		int codepoint = ((c & 0x07) << 18) |
		                ((p[0] & 0x3F) << 12) |
		                ((p[1] & 0x3F) << 6) |
		                (p[2] & 0x3F);
		*ptr = p + 3;
		return codepoint;
	}

	*ptr = p;
	return '?';
}

static bool atlas_supports_codepoint(int codepoint)
{
	return codepoint >= 32 && codepoint <= 255;
}

static bool font_needs_unicode_fallback(const char *text)
{
	const unsigned char *p = (const unsigned char *)text;

	while (*p) {
		int codepoint = utf8_next_codepoint(&p);
		if (!atlas_supports_codepoint(codepoint))
			return true;
	}

	return false;
}

/* ─── Software rendering target (Vulkan menu overlay) ─── */

static uint32_t *font_sw_buf = NULL;
static int font_sw_w = 0, font_sw_h = 0;

void font_set_sw_target(uint32_t *buf, int w, int h)
{
	font_sw_buf = buf;
	font_sw_w = w;
	font_sw_h = h;
}

void font_clear_sw_target(void)
{
	font_sw_buf = NULL;
}

static inline void sw_blend_pixel(int x, int y, font_color color, unsigned char coverage)
{
	uint32_t dst;
	unsigned int src_a;
	unsigned int inv_a;
	unsigned int src_r;
	unsigned int src_g;
	unsigned int src_b;
	unsigned int dst_r;
	unsigned int dst_g;
	unsigned int dst_b;
	unsigned int out_r;
	unsigned int out_g;
	unsigned int out_b;
	unsigned int out_a;

	if (x < 0 || x >= font_sw_w || y < 0 || y >= font_sw_h || coverage == 0)
		return;

	dst = font_sw_buf[y * font_sw_w + x];
	src_a = (unsigned int)(color.a * (float)coverage);
	inv_a = 255u - src_a;
	src_r = (unsigned int)(color.r * 255.0f);
	src_g = (unsigned int)(color.g * 255.0f);
	src_b = (unsigned int)(color.b * 255.0f);
	dst_r = (dst >> 16) & 0xFFu;
	dst_g = (dst >> 8) & 0xFFu;
	dst_b = dst & 0xFFu;
	out_r = (src_r * src_a + dst_r * inv_a) / 255u;
	out_g = (src_g * src_a + dst_g * inv_a) / 255u;
	out_b = (src_b * src_a + dst_b * inv_a) / 255u;
	out_a = src_a + (((dst >> 24) & 0xFFu) * inv_a) / 255u;

	font_sw_buf[y * font_sw_w + x] =
		(out_b & 0xFFu) |
		((out_g & 0xFFu) << 8) |
		((out_r & 0xFFu) << 16) |
		((out_a & 0xFFu) << 24);
}

static void font_render_text_sw(float x, float y, const char *text,
                                font_color color, float scale)
{
	if (!text || !text[0] || !temp_bitmap) return;

	float factor = (22.0f * scale) / 48.0f;
	float baseline_offset = 34.0f * factor;
	float cx = x;

	for (const unsigned char *p = (const unsigned char *)text; *p;) {
		int codepoint = utf8_next_codepoint(&p);
		if (!atlas_supports_codepoint(codepoint))
			codepoint = '?';

		int idx = codepoint - 32;
		int src_w = chardata[idx].x1 - chardata[idx].x0;
		int src_h = chardata[idx].y1 - chardata[idx].y0;
		int dst_w = (int)(src_w * factor + 0.5f);
		int dst_h = (int)(src_h * factor + 0.5f);

		float x_start = cx + chardata[idx].xoff * factor;
		float y_start = y + baseline_offset + chardata[idx].yoff * factor;

		if (dst_w > 0 && dst_h > 0) {
			for (int dy = 0; dy < dst_h; dy++) {
				int sy = dy * src_h / dst_h;
				int src_y = chardata[idx].y0 + sy;
				for (int dx = 0; dx < dst_w; dx++) {
					int sx = dx * src_w / dst_w;
					int src_x = chardata[idx].x0 + sx;
					unsigned char coverage = temp_bitmap[src_y * ATLAS_W + src_x];
					sw_blend_pixel((int)x_start + dx, (int)y_start + dy, color, coverage);
				}
			}
		}
		cx += chardata[idx].xadvance * factor;
	}
}

#ifdef _WIN32
static wchar_t *font_utf8_to_wide(const char *text);
static HFONT font_create_unicode_font(float scale);
static bool font_measure_unicode_text(const char *text, float scale, int *out_w, int *out_h);

static void font_render_unicode_text_sw(float x, float y, const char *text,
                                        font_color color, float scale)
{
	unicode_cache_entry_t *entry = find_unicode_cache_entry(text, scale);
	unsigned char *alpha = NULL;
	int tex_w = 0;
	int tex_h = 0;

	if (entry) {
		alpha = entry->alpha;
		tex_w = entry->width;
		tex_h = entry->height;
	} else {
		wchar_t *wide = font_utf8_to_wide(text);
		HDC dc = NULL;
		HFONT font = NULL;
		HGDIOBJ old_font = NULL;
		HBITMAP bmp = NULL;
		HGDIOBJ old_bmp = NULL;
		BITMAPINFO bi;
		void *bits = NULL;

		if (!font_sw_buf || !wide)
			return;

		if (!font_measure_unicode_text(text, scale, &tex_w, &tex_h)) {
			free(wide);
			return;
		}

		dc = CreateCompatibleDC(NULL);
		if (!dc)
			goto cleanup_gdi;

		font = font_create_unicode_font(scale);
		if (!font)
			goto cleanup_gdi;

		memset(&bi, 0, sizeof(bi));
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = tex_w;
		bi.bmiHeader.biHeight = -tex_h;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;

		bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
		if (!bmp || !bits)
			goto cleanup_gdi;

		old_bmp = SelectObject(dc, bmp);
		old_font = SelectObject(dc, font);

		SetBkMode(dc, OPAQUE);
		SetBkColor(dc, RGB(0, 0, 0));
		SetTextColor(dc, RGB(255, 255, 255));

		{
			RECT rect = {0, 0, tex_w, tex_h};
			HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
			if (brush) {
				FillRect(dc, &rect, brush);
				DeleteObject(brush);
			}
			rect.left = 1;
			rect.top = 1;
			DrawTextW(dc, wide, -1, &rect, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
		}

		GdiFlush();

		if (old_bmp) {
			SelectObject(dc, old_bmp);
			old_bmp = NULL;
		}

		alpha = (unsigned char *)malloc((size_t)(tex_w * tex_h));
		if (alpha) {
			const unsigned char *src = (const unsigned char *)bits;
			for (int i = 0; i < tex_w * tex_h; i++) {
				unsigned char b = src[i * 4 + 0];
				unsigned char g = src[i * 4 + 1];
				unsigned char r = src[i * 4 + 2];
				unsigned char a = r;
				if (g > a) a = g;
				if (b > a) a = b;
				alpha[i] = a;
			}

			unicode_cache_entry_t *new_entry = insert_unicode_cache_entry(text, scale, tex_w, tex_h, 0, alpha);
			alpha = new_entry->alpha;
		}

	cleanup_gdi:
		if (old_bmp)
			SelectObject(dc, old_bmp);
		if (old_font)
			SelectObject(dc, old_font);
		if (bmp)
			DeleteObject(bmp);
		if (font)
			DeleteObject(font);
		if (dc)
			DeleteDC(dc);
		free(wide);

		if (!alpha)
			return;
	}

	int base_x = (int)x;
	int base_y = (int)y;
	for (int yy = 0; yy < tex_h; yy++) {
		for (int xx = 0; xx < tex_w; xx++) {
			int i = yy * tex_w + xx;
			unsigned char a = alpha[i];
			sw_blend_pixel(base_x + xx, base_y + yy, color, a);
		}
	}
}

static wchar_t *font_utf8_to_wide(const char *text)
{
	int needed;
	wchar_t *wide;

	if (!text)
		return NULL;

	needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	if (needed <= 0)
		return NULL;

	wide = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
	if (!wide)
		return NULL;

	if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, needed) <= 0) {
		free(wide);
		return NULL;
	}

	return wide;
}

static HFONT font_create_unicode_font(float scale)
{
	int pixel_height = (int)(FONT_CHAR_H * scale + 0.5f);

	if (pixel_height < FONT_CHAR_H)
		pixel_height = FONT_CHAR_H;

	const wchar_t *face = L"Segoe UI";
	DWORD charset = DEFAULT_CHARSET;

	if (lang_current() == LANG_ZH) {
		face = L"Microsoft YaHei";
		charset = GB2312_CHARSET;
	} else if (lang_current() == LANG_HI) {
		face = L"Nirmala UI";
		charset = DEFAULT_CHARSET;
	}

	return CreateFontW(-pixel_height, 0, 0, 0, FW_NORMAL,
	                   FALSE, FALSE, FALSE, charset,
	                   OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
	                   ANTIALIASED_QUALITY,
	                   DEFAULT_PITCH | FF_DONTCARE, face);
}

static bool font_measure_unicode_text(const char *text, float scale, int *out_w, int *out_h)
{
	bool ok = false;
	wchar_t *wide = font_utf8_to_wide(text);
	HDC dc = NULL;
	HFONT font = NULL;
	HGDIOBJ old_font = NULL;
	RECT rect = {0, 0, 0, 0};
	TEXTMETRICW tm;
	int width = 0;
	int height = 0;

	if (!wide)
		return false;

	dc = CreateCompatibleDC(NULL);
	if (!dc)
		goto cleanup;

	font = font_create_unicode_font(scale);
	if (!font)
		goto cleanup;

	old_font = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);

	DrawTextW(dc, wide, -1, &rect, DT_CALCRECT | DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
	if (!GetTextMetricsW(dc, &tm))
		goto cleanup;

	width = rect.right - rect.left;
	height = tm.tmHeight;
	if (width < 1)
		width = 1;
	if (height < 1)
		height = FONT_CHAR_H;

	if (out_w)
		*out_w = width + 2;
	if (out_h)
		*out_h = height + 2;
	ok = true;

cleanup:
	if (old_font)
		SelectObject(dc, old_font);
	if (font)
		DeleteObject(font);
	if (dc)
		DeleteDC(dc);
	free(wide);
	return ok;
}

static void font_render_unicode_text(float x, float y, const char *text,
                                     font_color color, float scale,
                                     int screen_w, int screen_h)
{
	unicode_cache_entry_t *entry = find_unicode_cache_entry(text, scale);
	GLuint gl_tex = 0;
	int tex_w = 0;
	int tex_h = 0;

	if (entry) {
		gl_tex = entry->gl_tex;
		tex_w = entry->width;
		tex_h = entry->height;
	} else {
		wchar_t *wide = font_utf8_to_wide(text);
		HDC dc = NULL;
		HFONT font = NULL;
		HGDIOBJ old_font = NULL;
		HBITMAP bmp = NULL;
		HGDIOBJ old_bmp = NULL;
		BITMAPINFO bi;
		void *bits = NULL;
		unsigned char *alpha = NULL;

		if (!wide)
			return;

		if (!font_measure_unicode_text(text, scale, &tex_w, &tex_h)) {
			free(wide);
			return;
		}

		dc = CreateCompatibleDC(NULL);
		if (!dc)
			goto cleanup_gdi;

		font = font_create_unicode_font(scale);
		if (!font)
			goto cleanup_gdi;

		memset(&bi, 0, sizeof(bi));
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = tex_w;
		bi.bmiHeader.biHeight = -tex_h;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;

		bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
		if (!bmp || !bits)
			goto cleanup_gdi;

		old_bmp = SelectObject(dc, bmp);
		old_font = SelectObject(dc, font);

		SetBkMode(dc, OPAQUE);
		SetBkColor(dc, RGB(0, 0, 0));
		SetTextColor(dc, RGB(255, 255, 255));

		{
			RECT rect = {0, 0, tex_w, tex_h};
			HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
			if (brush) {
				FillRect(dc, &rect, brush);
				DeleteObject(brush);
			}
			rect.left = 1;
			rect.top = 1;
			DrawTextW(dc, wide, -1, &rect, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
		}

		GdiFlush();

		if (old_bmp) {
			SelectObject(dc, old_bmp);
			old_bmp = NULL;
		}

		alpha = (unsigned char *)malloc((size_t)(tex_w * tex_h));
		if (alpha) {
			const unsigned char *src = (const unsigned char *)bits;
			for (int i = 0; i < tex_w * tex_h; i++) {
				unsigned char b = src[i * 4 + 0];
				unsigned char g = src[i * 4 + 1];
				unsigned char r = src[i * 4 + 2];
				unsigned char a = r;
				if (g > a) a = g;
				if (b > a) a = b;
				alpha[i] = a;
			}

			glGenTextures(1, &gl_tex);
			glBindTexture(GL_TEXTURE_2D, gl_tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, tex_w, tex_h, 0, GL_RED, GL_UNSIGNED_BYTE, alpha);
			glBindTexture(GL_TEXTURE_2D, 0);

			free(alpha);

			insert_unicode_cache_entry(text, scale, tex_w, tex_h, gl_tex, NULL);
		}

	cleanup_gdi:
		if (old_bmp)
			SelectObject(dc, old_bmp);
		if (old_font)
			SelectObject(dc, old_font);
		if (bmp)
			DeleteObject(bmp);
		if (font)
			DeleteObject(font);
		if (dc)
			DeleteDC(dc);
		free(wide);

		if (gl_tex == 0)
			return;
	}

	float nx0 = (x / screen_w) * 2.0f - 1.0f;
	float ny0 = 1.0f - (y / screen_h) * 2.0f;
	float nx1 = ((x + tex_w) / screen_w) * 2.0f - 1.0f;
	float ny1 = 1.0f - ((y + tex_h) / screen_h) * 2.0f;

	float verts[] = {
		nx0, ny0, 0.0f, 0.0f,
		nx1, ny0, 1.0f, 0.0f,
		nx0, ny1, 0.0f, 1.0f,
		nx1, ny0, 1.0f, 0.0f,
		nx1, ny1, 1.0f, 1.0f,
		nx0, ny1, 0.0f, 1.0f,
	};

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(font_shader);
	glUniform1i(font_u_tex, 0);
	glUniform4f(font_u_color, color.r, color.g, color.b, color.a);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gl_tex);

	glBindVertexArray(font_vao);
	glBindBuffer(GL_ARRAY_BUFFER, font_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

	glEnableVertexAttribArray(font_i_pos);
	glEnableVertexAttribArray(font_i_coord);
	glVertexAttribPointer(font_i_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
	glVertexAttribPointer(font_i_coord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

	glDrawArrays(GL_TRIANGLES, 0, 6);

	glDisableVertexAttribArray(font_i_pos);
	glDisableVertexAttribArray(font_i_coord);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glUseProgram(0);
	glDisable(GL_BLEND);
}
#endif

static GLuint compile_font_shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char buf[512];
		glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
		fprintf(stderr, "Font shader error: %s\n", buf);
	}
	return s;
}

void font_init(void)
{
	temp_bitmap = (unsigned char *)malloc(ATLAS_W * ATLAS_H);
	if (!temp_bitmap) {
		fprintf(stderr, "Failed to allocate memory for font atlas bitmap.\n");
		return;
	}
	memset(temp_bitmap, 0, ATLAS_W * ATLAS_H);

	/* Bake Cabin font into atlas bitmap at height 48.0f */
	int result = stbtt_BakeFontBitmap(cabin_font_data, 0, 48.0f, temp_bitmap, ATLAS_W, ATLAS_H, 32, 224, chardata);
	if (result < 0) {
		fprintf(stderr, "Warning: font baking failed (returned %d). Font atlas might be too small.\n", result);
	}

	glGenTextures(1, &font_tex);
	glBindTexture(GL_TEXTURE_2D, font_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, ATLAS_W, ATLAS_H,
	             0, GL_RED, GL_UNSIGNED_BYTE, temp_bitmap);
	glBindTexture(GL_TEXTURE_2D, 0);

	/* Build shader */
	GLuint vs = compile_font_shader(GL_VERTEX_SHADER, font_vsrc);
	GLuint fs = compile_font_shader(GL_FRAGMENT_SHADER, font_fsrc);
	font_shader = glCreateProgram();
	glAttachShader(font_shader, vs);
	glAttachShader(font_shader, fs);
	glLinkProgram(font_shader);
	glDeleteShader(vs);
	glDeleteShader(fs);

	font_i_pos   = glGetAttribLocation(font_shader, "i_pos");
	font_i_coord = glGetAttribLocation(font_shader, "i_coord");
	font_u_tex   = glGetUniformLocation(font_shader, "u_tex");
	font_u_color = glGetUniformLocation(font_shader, "u_color");

	glGenVertexArrays(1, &font_vao);
	glGenBuffers(1, &font_vbo);
}

void font_deinit(void)
{
#ifdef _WIN32
	font_clear_unicode_cache();
#endif
	if (font_tex)     { glDeleteTextures(1, &font_tex); font_tex = 0; }
	if (font_vao)     { glDeleteVertexArrays(1, &font_vao); font_vao = 0; }
	if (font_vbo)     { glDeleteBuffers(1, &font_vbo); font_vbo = 0; }
	if (font_shader)  { glDeleteProgram(font_shader); font_shader = 0; }
	if (temp_bitmap)  { free(temp_bitmap); temp_bitmap = NULL; }
}

float font_text_width(const char *text, float scale)
{
	if (!text || !text[0])
		return 0.0f;

#ifdef _WIN32
	if (font_needs_unicode_fallback(text)) {
		unicode_cache_entry_t *entry = find_unicode_cache_entry(text, scale);
		if (entry) {
			return (float)entry->width;
		}
		int width = 0;
		if (font_measure_unicode_text(text, scale, &width, NULL))
			return (float)width;
	}
#endif

	float factor = (22.0f * scale) / 48.0f;
	float width = 0.0f;

	for (const unsigned char *p = (const unsigned char *)text; *p;) {
		int codepoint = utf8_next_codepoint(&p);
		if (!atlas_supports_codepoint(codepoint))
			codepoint = '?';

		int idx = codepoint - 32;
		width += chardata[idx].xadvance * factor;
	}
	return width;
}

float font_text_height(float scale)
{
	return 22.0f * scale;
}

typedef struct {
	GLint program;
	GLint active_texture;
	GLint texture;
	GLint vao;
	GLint vbo;
	GLint unpack_row_length;
	GLint unpack_alignment;
	GLint viewport[4];
	GLboolean blend_enabled;
	GLint blend_src_rgb;
	GLint blend_dst_rgb;
	GLint blend_src_alpha;
	GLint blend_dst_alpha;
	GLint blend_eq_rgb;
	GLint blend_eq_alpha;
} font_gl_state_t;

static void font_push_gl_state(font_gl_state_t *state)
{
	glGetIntegerv(GL_CURRENT_PROGRAM, &state->program);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state->vao);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state->vbo);
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &state->unpack_row_length);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &state->unpack_alignment);
	glGetIntegerv(GL_VIEWPORT, state->viewport);
	state->blend_enabled = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_BLEND_SRC_RGB, &state->blend_src_rgb);
	glGetIntegerv(GL_BLEND_DST_RGB, &state->blend_dst_rgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &state->blend_src_alpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &state->blend_dst_alpha);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &state->blend_eq_rgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &state->blend_eq_alpha);
}

static void font_pop_gl_state(const font_gl_state_t *state)
{
	glUseProgram(state->program);
	glActiveTexture(state->active_texture);
	glBindTexture(GL_TEXTURE_2D, state->texture);
	glBindVertexArray(state->vao);
	glBindBuffer(GL_ARRAY_BUFFER, state->vbo);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, state->unpack_row_length);
	glPixelStorei(GL_UNPACK_ALIGNMENT, state->unpack_alignment);
	glViewport(state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
	if (state->blend_enabled)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);
	glBlendFuncSeparate(state->blend_src_rgb, state->blend_dst_rgb,
	                    state->blend_src_alpha, state->blend_dst_alpha);
	glBlendEquationSeparate(state->blend_eq_rgb, state->blend_eq_alpha);
}

void font_render_text(float x, float y, const char *text,
                      font_color color, float scale,
                      int screen_w, int screen_h)
{
	if (!text || !text[0]) return;

	if (font_sw_buf) {
#ifdef _WIN32
		if (font_needs_unicode_fallback(text)) {
			font_render_unicode_text_sw(x, y, text, color, scale);
			return;
		}
#endif
		font_render_text_sw(x, y, text, color, scale);
		return;
	}

	if (!font_shader) return;

	int len = (int)strlen(text);
	int max_verts = len * 6; /* 2 triangles per char */
	float *verts = (float*)malloc(max_verts * 4 * sizeof(float)); /* x,y,u,v */
	if (!verts) return;

	font_gl_state_t gl_state;
	font_push_gl_state(&gl_state);

#ifdef _WIN32
	if (font_needs_unicode_fallback(text)) {
		font_render_unicode_text(x, y, text, color, scale, screen_w, screen_h);
		free(verts);
		font_pop_gl_state(&gl_state);
		return;
	}
#endif

	int vi = 0;
	float factor = (22.0f * scale) / 48.0f;
	float baseline_offset = 34.0f * factor;
	float cx = x;

	for (const unsigned char *p = (const unsigned char *)text; *p;) {
		int codepoint = utf8_next_codepoint(&p);
		if (!atlas_supports_codepoint(codepoint))
			codepoint = '?';

		int idx = codepoint - 32;

		stbtt_aligned_quad q;
		float rx = 0.0f;
		float ry = 0.0f;
		stbtt_GetBakedQuad(chardata, ATLAS_W, ATLAS_H, idx, &rx, &ry, &q, 1);

		float next_cx = cx + rx * factor;

		float x0 = cx + q.x0 * factor;
		float y0 = y + baseline_offset + q.y0 * factor;
		float x1 = cx + q.x1 * factor;
		float y1 = y + baseline_offset + q.y1 * factor;

		/* Convert pixel coords to NDC (-1..1) */
		float nx0 = (x0 / screen_w) * 2.0f - 1.0f;
		float ny0 = 1.0f - (y0 / screen_h) * 2.0f;
		float nx1 = (x1 / screen_w) * 2.0f - 1.0f;
		float ny1 = 1.0f - (y1 / screen_h) * 2.0f;

		/* Triangle 1 */
		verts[vi++] = nx0; verts[vi++] = ny0; verts[vi++] = q.s0; verts[vi++] = q.t0;
		verts[vi++] = nx1; verts[vi++] = ny0; verts[vi++] = q.s1; verts[vi++] = q.t0;
		verts[vi++] = nx0; verts[vi++] = ny1; verts[vi++] = q.s0; verts[vi++] = q.t1;
		/* Triangle 2 */
		verts[vi++] = nx1; verts[vi++] = ny0; verts[vi++] = q.s1; verts[vi++] = q.t0;
		verts[vi++] = nx1; verts[vi++] = ny1; verts[vi++] = q.s1; verts[vi++] = q.t1;
		verts[vi++] = nx0; verts[vi++] = ny1; verts[vi++] = q.s0; verts[vi++] = q.t1;

		cx = next_cx;
	}

	/* Render */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(font_shader);
	glUniform1i(font_u_tex, 0);
	glUniform4f(font_u_color, color.r, color.g, color.b, color.a);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, font_tex);

	glBindVertexArray(font_vao);
	glBindBuffer(GL_ARRAY_BUFFER, font_vbo);
	glBufferData(GL_ARRAY_BUFFER, vi * sizeof(float), verts, GL_DYNAMIC_DRAW);

	glEnableVertexAttribArray(font_i_pos);
	glEnableVertexAttribArray(font_i_coord);
	glVertexAttribPointer(font_i_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
	glVertexAttribPointer(font_i_coord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

	glDrawArrays(GL_TRIANGLES, 0, vi / 4);

	glDisableVertexAttribArray(font_i_pos);
	glDisableVertexAttribArray(font_i_coord);
	free(verts);

	font_pop_gl_state(&gl_state);
}
