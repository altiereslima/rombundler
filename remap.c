/*
 * remap.c — Remapeamento de controle por jogo (modelo unificado).
 *
 * Cada botão RetroPad (0..15) aponta para UM código físico: um botão GLFW
 * (0..LAST), um gatilho analógico (REMAP_PHYS_LT/RT) ou nada (REMAP_PHYS_NONE).
 * Isso permite mapear qualquer botão emulado para qualquer entrada física,
 * inclusive os gatilhos — e vice-versa.
 *
 * Salva em ./saves/<rom>.remap.ini. Formato v2:
 *   [meta]
 *   version = 2
 *   [port0]
 *   button_0 = 0      ; RetroPad B  -> GLFW A
 *   button_12 = 256   ; RetroPad L2 -> gatilho LT (REMAP_PHYS_LT)
 *   analog_dpad = 1
 *   ...
 *
 * Arquivos antigos (sem version, ou version 1) são migrados ao carregar:
 * os antigos campos trigger_l2/trigger_r2 e os valores inertes de button_12/13
 * são convertidos para o modelo unificado, preservando o comportamento sentido
 * pelo usuário (LT->L2, RT->R2 por padrão).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "remap.h"
#include "ini.h"
#include "libretro.h"

#define REMAP_FORMAT_VERSION 2
#define LEGACY_TRIGGER_UNSET 0xFFFFu

/* Tabela de remap: [porta][botão retro] = código físico */
static unsigned remap_table[REMAP_MAX_PORTS][REMAP_MAX_BUTTONS];
/* Modo analógico→dpad por porta: -1=herda global, 0=off, 1=esq, 2=dir, 3=ambos */
static int port_analog_dpad[REMAP_MAX_PORTS];

/* Estado de carregamento / migração */
static int      loaded_version = REMAP_FORMAT_VERSION;
static unsigned legacy_trigger_l2[REMAP_MAX_PORTS];
static unsigned legacy_trigger_r2[REMAP_MAX_PORTS];

static char remap_filepath[512] = {0};
static char legacy_remap_filepath[512] = {0};
static bool remap_loaded = false;

/* Mapeamento padrão RetroPad -> físico, na ordem oficial libretro.
 * L2/R2 apontam para os gatilhos analógicos por padrão. */
static const unsigned default_map[REMAP_MAX_BUTTONS] = {
	[RETRO_DEVICE_ID_JOYPAD_B]      = GLFW_GAMEPAD_BUTTON_A,
	[RETRO_DEVICE_ID_JOYPAD_Y]      = GLFW_GAMEPAD_BUTTON_X,
	[RETRO_DEVICE_ID_JOYPAD_SELECT] = GLFW_GAMEPAD_BUTTON_BACK,
	[RETRO_DEVICE_ID_JOYPAD_START]  = GLFW_GAMEPAD_BUTTON_START,
	[RETRO_DEVICE_ID_JOYPAD_UP]     = GLFW_GAMEPAD_BUTTON_DPAD_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN]   = GLFW_GAMEPAD_BUTTON_DPAD_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT]   = GLFW_GAMEPAD_BUTTON_DPAD_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT]  = GLFW_GAMEPAD_BUTTON_DPAD_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A]      = GLFW_GAMEPAD_BUTTON_B,
	[RETRO_DEVICE_ID_JOYPAD_X]      = GLFW_GAMEPAD_BUTTON_Y,
	[RETRO_DEVICE_ID_JOYPAD_L]      = GLFW_GAMEPAD_BUTTON_LEFT_BUMPER,
	[RETRO_DEVICE_ID_JOYPAD_R]      = GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER,
	[RETRO_DEVICE_ID_JOYPAD_L2]     = REMAP_PHYS_LT,
	[RETRO_DEVICE_ID_JOYPAD_R2]     = REMAP_PHYS_RT,
	[RETRO_DEVICE_ID_JOYPAD_L3]     = GLFW_GAMEPAD_BUTTON_LEFT_THUMB,
	[RETRO_DEVICE_ID_JOYPAD_R3]     = GLFW_GAMEPAD_BUTTON_RIGHT_THUMB,
};

static bool remap_has_custom_mappings(void)
{
	for (int port = 0; port < REMAP_MAX_PORTS; port++) {
		for (int i = 0; i < REMAP_MAX_BUTTONS; i++)
			if (remap_table[port][i] != default_map[i])
				return true;
		if (port_analog_dpad[port] >= 0)
			return true;
	}
	return false;
}

/* Gera os caminhos novo (./saves) e legado (ao lado da ROM). */
static void build_remap_path(const char *rom_path)
{
	if (!rom_path || !rom_path[0]) {
		remap_filepath[0] = '\0';
		legacy_remap_filepath[0] = '\0';
		return;
	}

	strncpy(legacy_remap_filepath, rom_path, sizeof(legacy_remap_filepath) - 20);
	legacy_remap_filepath[sizeof(legacy_remap_filepath) - 20] = '\0';
	char *legacy_dot = strrchr(legacy_remap_filepath, '.');
	if (legacy_dot)
		*legacy_dot = '\0';
	strcat(legacy_remap_filepath, ".remap.ini");

	const char *base = rom_path;
	for (const char *p = rom_path; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;

	char name[256];
	strncpy(name, base, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	char *dot = strrchr(name, '.');
	if (dot)
		*dot = '\0';

	MKDIR("./saves");
	snprintf(remap_filepath, sizeof(remap_filepath), "./saves/%s.remap.ini", name);
}

static int remap_ini_handler(void *user, const char *section,
                             const char *name, const char *value)
{
	(void)user;

	if (strcmp(section, "meta") == 0) {
		if (strcmp(name, "version") == 0)
			loaded_version = atoi(value);
		return 1;
	}

	int port = -1;
	if (sscanf(section, "port%d", &port) != 1 || port < 0 || port >= REMAP_MAX_PORTS)
		return 1;

	/* Campos legados (v1) — guardados para migração */
	if (strcmp(name, "trigger_l2") == 0) {
		int v = atoi(value);
		if (v >= 0 && v < REMAP_MAX_BUTTONS)
			legacy_trigger_l2[port] = (unsigned)v;
		return 1;
	}
	if (strcmp(name, "trigger_r2") == 0) {
		int v = atoi(value);
		if (v >= 0 && v < REMAP_MAX_BUTTONS)
			legacy_trigger_r2[port] = (unsigned)v;
		return 1;
	}
	if (strcmp(name, "analog_dpad") == 0) {
		int v = atoi(value);
		if (v >= 0 && v <= 3)
			port_analog_dpad[port] = v;
		return 1;
	}

	int btn_id = -1;
	if (sscanf(name, "button_%d", &btn_id) != 1 ||
	    btn_id < 0 || btn_id >= REMAP_MAX_BUTTONS)
		return 1;

	remap_table[port][btn_id] = (unsigned)strtoul(value, NULL, 10);
	return 1;
}

/* Converte um arquivo v1 (modelo de "trigger target" separado) para o modelo
 * unificado. Em v1, button_12/13 (L2/R2) eram inertes e o gatilho físico LT/RT
 * ia para trigger_l2/r2 (padrão L2/R2). */
static void remap_migrate_legacy_v1(void)
{
	for (int port = 0; port < REMAP_MAX_PORTS; port++) {
		/* Valores antigos de L2/R2 não tinham efeito → restaura gatilhos. */
		remap_table[port][RETRO_DEVICE_ID_JOYPAD_L2] = REMAP_PHYS_LT;
		remap_table[port][RETRO_DEVICE_ID_JOYPAD_R2] = REMAP_PHYS_RT;

		unsigned tl = legacy_trigger_l2[port];
		if (tl != LEGACY_TRIGGER_UNSET && tl < REMAP_MAX_BUTTONS &&
		    tl != RETRO_DEVICE_ID_JOYPAD_L2) {
			remap_table[port][tl] = REMAP_PHYS_LT;
			remap_table[port][RETRO_DEVICE_ID_JOYPAD_L2] = REMAP_PHYS_NONE;
		}

		unsigned tr = legacy_trigger_r2[port];
		if (tr != LEGACY_TRIGGER_UNSET && tr < REMAP_MAX_BUTTONS &&
		    tr != RETRO_DEVICE_ID_JOYPAD_R2) {
			remap_table[port][tr] = REMAP_PHYS_RT;
			remap_table[port][RETRO_DEVICE_ID_JOYPAD_R2] = REMAP_PHYS_NONE;
		}
	}
}

void remap_reset_defaults(int port)
{
	if (port < 0 || port >= REMAP_MAX_PORTS) return;
	memcpy(remap_table[port], default_map, sizeof(default_map));
	port_analog_dpad[port] = -1;
}

void remap_reset_all_defaults(void)
{
	for (int p = 0; p < REMAP_MAX_PORTS; p++)
		remap_reset_defaults(p);
}

void remap_init(const char *rom_path)
{
	remap_reset_all_defaults();
	build_remap_path(rom_path);
	remap_load();
}

void remap_load(void)
{
	remap_reset_all_defaults();
	loaded_version = REMAP_FORMAT_VERSION;
	for (int p = 0; p < REMAP_MAX_PORTS; p++) {
		legacy_trigger_l2[p] = LEGACY_TRIGGER_UNSET;
		legacy_trigger_r2[p] = LEGACY_TRIGGER_UNSET;
	}

	if (!remap_filepath[0]) return;

	bool parsed = false;
	if (ini_parse(remap_filepath, remap_ini_handler, NULL) == 0) {
		parsed = true;
		printf("Remap carregado: %s\n", remap_filepath);
	} else if (legacy_remap_filepath[0] &&
	           ini_parse(legacy_remap_filepath, remap_ini_handler, NULL) == 0) {
		parsed = true;
		printf("Remap legado carregado: %s\n", legacy_remap_filepath);
	} else {
		printf("Sem arquivo de remap (usando padrao): %s\n", remap_filepath);
	}

	if (parsed) {
		if (loaded_version < 2)
			remap_migrate_legacy_v1();
		remap_loaded = true;
	}
}

void remap_save(void)
{
	if (!remap_filepath[0]) return;

	if (!remap_has_custom_mappings()) {
		if (remove(remap_filepath) == 0)
			printf("Remap padrao removido: %s\n", remap_filepath);
		if (legacy_remap_filepath[0] && remove(legacy_remap_filepath) == 0)
			printf("Remap legado removido: %s\n", legacy_remap_filepath);
		remap_loaded = false;
		return;
	}

	FILE *f = fopen(remap_filepath, "w");
	if (!f) {
		fprintf(stderr, "Erro ao salvar remap: %s\n", remap_filepath);
		return;
	}

	fprintf(f, "[meta]\nversion = %d\n\n", REMAP_FORMAT_VERSION);

	for (int port = 0; port < REMAP_MAX_PORTS; port++) {
		fprintf(f, "[port%d]\n", port);
		for (int i = 0; i < REMAP_MAX_BUTTONS; i++)
			fprintf(f, "button_%d = %u\n", i, remap_table[port][i]);
		if (port_analog_dpad[port] >= 0)
			fprintf(f, "analog_dpad = %d\n", port_analog_dpad[port]);
		fprintf(f, "\n");
	}

	fclose(f);
	remap_loaded = true;
	printf("Remap salvo: %s\n", remap_filepath);
}

void remap_set(int port, unsigned retro_id, unsigned phys_code)
{
	if (port < 0 || port >= REMAP_MAX_PORTS) return;
	if (retro_id >= REMAP_MAX_BUTTONS) return;
	remap_table[port][retro_id] = phys_code;
}

unsigned remap_get(int port, unsigned retro_id)
{
	if (port < 0 || port >= REMAP_MAX_PORTS) return REMAP_PHYS_NONE;
	if (retro_id >= REMAP_MAX_BUTTONS) return REMAP_PHYS_NONE;
	return remap_table[port][retro_id];
}

int remap_get_analog_dpad_mode(int port)
{
	if (port < 0 || port >= REMAP_MAX_PORTS) return -1;
	return port_analog_dpad[port];
}
