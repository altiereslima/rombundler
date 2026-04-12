/*
 * remap.c — Sistema de remapeamento de controle por jogo.
 *
 * Salva/carrega mapeamentos individuais por ROM em ./saves/<rom>.remap.ini,
 * ao lado dos ajustes de aspecto por jogo. Suporta todas as 4 portas.
 *
 * Formato do arquivo:
 *   [port0]
 *   button_0 = 0    ; RetroPad B      → GLFW button A
 *   button_3 = 7    ; RetroPad Start  → GLFW button Start
 *   button_8 = 1    ; RetroPad A      → GLFW button B
 *   ...
 *   [port1]
 *   ...
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

/* Tabela de remap: [porta][botão retro] = botão GLFW */
static unsigned remap_table[REMAP_MAX_PORTS][REMAP_MAX_BUTTONS];
static char remap_filepath[512] = {0};
static char legacy_remap_filepath[512] = {0};
static bool remap_loaded = false;

/* Mapeamento padrão RetroPad -> GLFW Gamepad, seguindo a ordem oficial libretro. */
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
	[RETRO_DEVICE_ID_JOYPAD_L2]     = 0, /* eixo, não botão */
	[RETRO_DEVICE_ID_JOYPAD_R2]     = 0, /* eixo, não botão */
	[RETRO_DEVICE_ID_JOYPAD_L3]     = GLFW_GAMEPAD_BUTTON_LEFT_THUMB,
	[RETRO_DEVICE_ID_JOYPAD_R3]     = GLFW_GAMEPAD_BUTTON_RIGHT_THUMB,
};

static bool remap_has_custom_mappings(void)
{
	for (int port = 0; port < REMAP_MAX_PORTS; port++) {
		for (int i = 0; i < REMAP_MAX_BUTTONS; i++) {
			if (remap_table[port][i] != default_map[i])
				return true;
		}
	}

	return false;
}

/* Gera os caminhos novo e legado do remap por jogo. */
static void build_remap_path(const char *rom_path)
{
	if (!rom_path || !rom_path[0]) {
		remap_filepath[0] = '\0';
		legacy_remap_filepath[0] = '\0';
		return;
	}

	/* Caminho legado: ao lado da ROM */
	strncpy(legacy_remap_filepath, rom_path, sizeof(legacy_remap_filepath) - 20);
	legacy_remap_filepath[sizeof(legacy_remap_filepath) - 20] = '\0';
	char *legacy_dot = strrchr(legacy_remap_filepath, '.');
	if (legacy_dot)
		*legacy_dot = '\0';
	strcat(legacy_remap_filepath, ".remap.ini");

	/* Encontra apenas o basename para salvar em ./saves */
	const char *base = rom_path;
	const char *p = rom_path;
	while (*p) {
		if (*p == '/' || *p == '\\')
			base = p + 1;
		p++;
	}

	char name[256];
	strncpy(name, base, sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	char *dot = strrchr(name, '.');
	if (dot)
		*dot = '\0';

	MKDIR("./saves");
	snprintf(remap_filepath, sizeof(remap_filepath), "./saves/%s.remap.ini", name);
}

/* Callback do parser INI */
static int remap_ini_handler(void *user, const char *section,
                             const char *name, const char *value)
{
	(void)user;

	/* Determina a porta pela seção */
	int port = -1;
	if (sscanf(section, "port%d", &port) != 1 || port < 0 || port >= REMAP_MAX_PORTS)
		return 1;

	/* Determina o botão pelo nome */
	int btn_id = -1;
	if (sscanf(name, "button_%d", &btn_id) != 1 || btn_id < 0 || btn_id >= REMAP_MAX_BUTTONS)
		return 1;

	remap_table[port][btn_id] = (unsigned)atoi(value);
	return 1;
}

void remap_reset_defaults(int port)
{
	if (port < 0 || port >= REMAP_MAX_PORTS) return;
	memcpy(remap_table[port], default_map, sizeof(default_map));
}

void remap_reset_all_defaults(void)
{
	for (int p = 0; p < REMAP_MAX_PORTS; p++)
		remap_reset_defaults(p);
}

void remap_init(const char *rom_path)
{
	/* Primeiro define os padrões para todas as portas */
	remap_reset_all_defaults();

	/* Constrói o caminho do arquivo de remap */
	build_remap_path(rom_path);

	/* Tenta carregar */
	remap_load();
}

void remap_load(void)
{
	if (!remap_filepath[0]) return;

	if (ini_parse(remap_filepath, remap_ini_handler, NULL) == 0) {
		remap_loaded = true;
		printf("Remap carregado: %s\n", remap_filepath);
	} else if (legacy_remap_filepath[0] &&
	           ini_parse(legacy_remap_filepath, remap_ini_handler, NULL) == 0) {
		remap_loaded = true;
		printf("Remap legado carregado: %s\n", legacy_remap_filepath);
	} else {
		printf("Sem arquivo de remap (usando padrao): %s\n", remap_filepath);
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

	for (int port = 0; port < REMAP_MAX_PORTS; port++) {
		fprintf(f, "[port%d]\n", port);
		for (int i = 0; i < REMAP_MAX_BUTTONS; i++) {
			fprintf(f, "button_%d = %u\n", i, remap_table[port][i]);
		}
		fprintf(f, "\n");
	}

	fclose(f);
	remap_loaded = true;
	printf("Remap salvo: %s\n", remap_filepath);
}

void remap_set(int port, unsigned retro_id, unsigned glfw_button)
{
	if (port < 0 || port >= REMAP_MAX_PORTS) return;
	if (retro_id >= REMAP_MAX_BUTTONS) return;
	remap_table[port][retro_id] = glfw_button;
}

unsigned remap_get(int port, unsigned retro_id)
{
	if (port < 0 || port >= REMAP_MAX_PORTS) return 0;
	if (retro_id >= REMAP_MAX_BUTTONS) return 0;
	return remap_table[port][retro_id];
}
