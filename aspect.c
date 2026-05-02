/*
 * aspect.c — Sistema de proporção de tela por jogo.
 *
 * Salva/carrega em ./saves/<rom_name>.aspect
 * Suporta modos Core, Stretch, 4:3, 16:9 e Custom.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aspect.h"
#include "lang.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

static aspect_mode_t current_mode = ASPECT_CORE;
static aspect_custom_t custom = {0, 0, 0, 0, 100};
static char save_path[512] = {0};

/* Gera o caminho do arquivo de aspecto: ./saves/<rom_basename>.aspect */
static void make_save_path(const char *rom_path)
{
	if (!rom_path) {
		save_path[0] = '\0';
		return;
	}

	/* Encontra o basename da ROM */
	const char *base = rom_path;
	const char *p = rom_path;
	while (*p) {
		if (*p == '/' || *p == '\\') base = p + 1;
		p++;
	}

	/* Cria diretório de saves */
	MKDIR("./saves");

	/* Remove a extensão do arquivo */
	char name[256];
	strncpy(name, base, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	char *dot = strrchr(name, '.');
	if (dot) *dot = '\0';

	snprintf(save_path, sizeof(save_path), "./saves/%s.aspect", name);
}

static void load_from_file(void)
{
	if (save_path[0] == '\0') return;

	FILE *f = fopen(save_path, "r");
	if (!f) return;

	int mode_int = 0;
	if (fscanf(f, "mode=%d\n", &mode_int) == 1) {
		if (mode_int >= 0 && mode_int < ASPECT_COUNT)
			current_mode = (aspect_mode_t)mode_int;
	}
	fscanf(f, "off_x=%d\n", &custom.off_x);
	fscanf(f, "off_y=%d\n", &custom.off_y);
	fscanf(f, "adj_w=%d\n", &custom.adj_w);
	fscanf(f, "adj_h=%d\n", &custom.adj_h);
	int zoom_pct = 100;
	if (fscanf(f, "zoom_pct=%d\n", &zoom_pct) == 1) {
		if (zoom_pct < 10) zoom_pct = 10;
		if (zoom_pct > 500) zoom_pct = 500;
		custom.zoom_pct = zoom_pct;
	}

	fclose(f);
	printf("Aspect: carregado %s (modo=%d zoom_pct=%d)\n", save_path, current_mode, custom.zoom_pct);
}

void aspect_init(const char *rom_path)
{
	current_mode = ASPECT_CORE;
	custom.off_x = 0;
	custom.off_y = 0;
	custom.adj_w = 0;
	custom.adj_h = 0;
	custom.zoom_pct = 100;
	make_save_path(rom_path);
	load_from_file();
}

void aspect_save(void)
{
	if (save_path[0] == '\0') return;

	FILE *f = fopen(save_path, "w");
	if (!f) {
		fprintf(stderr, "Aspect: nao foi possivel salvar %s\n", save_path);
		return;
	}

	fprintf(f, "mode=%d\n", (int)current_mode);
	fprintf(f, "off_x=%d\n", custom.off_x);
	fprintf(f, "off_y=%d\n", custom.off_y);
	fprintf(f, "adj_w=%d\n", custom.adj_w);
	fprintf(f, "adj_h=%d\n", custom.adj_h);
	fprintf(f, "zoom_pct=%d\n", custom.zoom_pct);

	fclose(f);
}

aspect_mode_t aspect_get_mode(void)
{
	return current_mode;
}

void aspect_set_mode(aspect_mode_t mode)
{
	if (mode >= 0 && mode < ASPECT_COUNT)
		current_mode = mode;
}

void aspect_cycle(int dir)
{
	int m = (int)current_mode + dir;
	if (m < 0) m = ASPECT_COUNT - 1;
	if (m >= ASPECT_COUNT) m = 0;
	current_mode = (aspect_mode_t)m;
}

const char *aspect_mode_name(aspect_mode_t mode)
{
	switch (mode) {
		case ASPECT_CORE:          return lang_get(STR_ASPECT_CORE);
		case ASPECT_STRETCH:       return lang_get(STR_ASPECT_STRETCH);
		case ASPECT_4_3:           return lang_get(STR_ASPECT_4_3);
		case ASPECT_16_9:          return lang_get(STR_ASPECT_16_9);
		case ASPECT_CUSTOM:        return lang_get(STR_ASPECT_CUSTOM);
		case ASPECT_ZOOM:    return lang_get(STR_ASPECT_ZOOM);
		default:                   return "???";
	}
}

aspect_viewport_t aspect_calc(int win_w, int win_h, int core_w, int core_h, float core_aspect)
{
	aspect_viewport_t vp = {0, 0, win_w, win_h};

	if (current_mode == ASPECT_ZOOM) {
		/* Calcula o viewport normal (Core) e aplica zoom + offset */
		float ratio = core_aspect;
		if (ratio <= 0.0f && core_w > 0 && core_h > 0)
			ratio = (float)core_w / (float)core_h;
		if (ratio <= 0.0f) ratio = 4.0f / 3.0f;

		float win_ratio = (float)win_w / (float)win_h;
		int base_w, base_h;

		if (win_ratio > ratio) {
			base_h = win_h;
			base_w = (int)(win_h * ratio + 0.5f);
		} else {
			base_w = win_w;
			base_h = (int)(win_w / ratio + 0.5f);
		}

		float factor = custom.zoom_pct / 100.0f;
		vp.w = (int)(base_w * factor);
		vp.h = (int)(base_h * factor);
		vp.x = (win_w - vp.w) / 2 + custom.off_x;
		vp.y = (win_h - vp.h) / 2 + custom.off_y;
		return vp;
	}

	if (current_mode == ASPECT_STRETCH) {
		/* Nada a fazer — já cobre toda a janela */
		goto apply_custom;
	}

	float target_ratio;
	switch (current_mode) {
		case ASPECT_4_3:
			target_ratio = 4.0f / 3.0f;
			break;
		case ASPECT_16_9:
			target_ratio = 16.0f / 9.0f;
			break;
		case ASPECT_CORE:
		case ASPECT_CUSTOM:
		default:
			if (core_aspect > 0.0f)
				target_ratio = core_aspect;
			else if (core_w > 0 && core_h > 0)
				target_ratio = (float)core_w / (float)core_h;
			else
				target_ratio = 4.0f / 3.0f; /* Fallback */
			break;
	}

	float win_ratio = (float)win_w / (float)win_h;

	if (win_ratio > target_ratio) {
		/* Janela mais larga que o conteúdo — barras laterais */
		vp.h = win_h;
		vp.w = (int)(win_h * target_ratio + 0.5f);
		vp.x = (win_w - vp.w) / 2;
		vp.y = 0;
	} else {
		/* Janela mais alta — barras horizontal */
		vp.w = win_w;
		vp.h = (int)(win_w / target_ratio + 0.5f);
		vp.x = 0;
		vp.y = (win_h - vp.h) / 2;
	}

apply_custom:
	if (current_mode == ASPECT_CUSTOM) {
		vp.x += custom.off_x;
		vp.y += custom.off_y;
		vp.w += custom.adj_w;
		vp.h += custom.adj_h;
		/* Clamp mínimos */
		if (vp.w < 64) vp.w = 64;
		if (vp.h < 64) vp.h = 64;
	}

	return vp;
}

aspect_custom_t *aspect_get_custom(void)
{
	return &custom;
}

void aspect_custom_reset(void)
{
	custom.off_x = 0;
	custom.off_y = 0;
	custom.adj_w = 0;
	custom.adj_h = 0;
	custom.zoom_pct = 100;
}

int aspect_zoom_pct(void)
{
	return custom.zoom_pct;
}

void aspect_zoom_delta(int delta)
{
	custom.zoom_pct += delta;
	if (custom.zoom_pct < 10) custom.zoom_pct = 10;
	if (custom.zoom_pct > 500) custom.zoom_pct = 500;
}
