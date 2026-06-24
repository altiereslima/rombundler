/*
 * input_profile.c — Perfis de controle (fallback) e perfis por jogo.
 * Tabelas validadas em controles_por_core.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "input_profile.h"
#include "ini.h"
#include "libretro.h"
#include "lang.h"

#define GP_MAX_PORTS   4
#define GP_MAX_BUTTONS 16

/* ─────────────────── Helpers ─────────────────── */

static bool contains_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !needle[0])
		return false;
	for (; *haystack; haystack++) {
		const char *h = haystack;
		const char *n = needle;
		while (*h && *n &&
		       tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
			h++;
			n++;
		}
		if (*n == '\0')
			return true;
	}
	return false;
}

/* ─────────────────── input_profile (fallback) ─────────────────── */

int input_profile_parse(const char *s)
{
	if (!s || !s[0] || strcmp(s, "auto") == 0)
		return -1;

	if (!strcmp(s, "generic"))                       return INPUT_PROFILE_GENERIC;
	if (!strcmp(s, "master_system") || !strcmp(s, "sms") ||
	    !strcmp(s, "game_gear") || !strcmp(s, "gg"))  return INPUT_PROFILE_MASTER_SYSTEM;
	if (!strcmp(s, "megadrive_3"))                    return INPUT_PROFILE_MEGADRIVE_3;
	if (!strcmp(s, "megadrive_6") || !strcmp(s, "megadrive") ||
	    !strcmp(s, "genesis"))                        return INPUT_PROFILE_MEGADRIVE_6;
	if (!strcmp(s, "snes"))                           return INPUT_PROFILE_SNES;
	if (!strcmp(s, "nes"))                            return INPUT_PROFILE_NES;
	if (!strcmp(s, "gb") || !strcmp(s, "gameboy") ||
	    !strcmp(s, "gbc"))                            return INPUT_PROFILE_GB;
	if (!strcmp(s, "gba"))                            return INPUT_PROFILE_GBA;
	if (!strcmp(s, "pc_engine") || !strcmp(s, "pce") ||
	    !strcmp(s, "turbografx"))                     return INPUT_PROFILE_PC_ENGINE;
	if (!strcmp(s, "neogeo") || !strcmp(s, "arcade")) return INPUT_PROFILE_NEOGEO;
	if (!strcmp(s, "jaguar"))                         return INPUT_PROFILE_JAGUAR;
	if (!strcmp(s, "saturn"))                         return INPUT_PROFILE_SATURN;
	if (!strcmp(s, "n64"))                            return INPUT_PROFILE_N64;
	if (!strcmp(s, "psx") || !strcmp(s, "playstation") ||
	    !strcmp(s, "ps1"))                            return INPUT_PROFILE_PSX;
	if (!strcmp(s, "dreamcast") || !strcmp(s, "dc"))  return INPUT_PROFILE_DREAMCAST;
	if (!strcmp(s, "psp"))                            return INPUT_PROFILE_PSP;

	return -1;
}

input_profile_t input_profile_detect(const char *core_path,
                                     const char *rom_path,
                                     const char *config_value)
{
	int parsed = input_profile_parse(config_value);
	(void)rom_path;

	if (parsed >= 0)
		return (input_profile_t)parsed;

	if (!core_path)
		return INPUT_PROFILE_GENERIC;

	if (contains_ci(core_path, "virtualjaguar"))     return INPUT_PROFILE_JAGUAR;
	if (contains_ci(core_path, "gearsystem") ||
	    contains_ci(core_path, "smsplus"))           return INPUT_PROFILE_MASTER_SYSTEM;
	if (contains_ci(core_path, "genesis_plus_gx") ||
	    contains_ci(core_path, "genesisplusgx") ||
	    contains_ci(core_path, "blastem") ||
	    contains_ci(core_path, "picodrive"))         return INPUT_PROFILE_MEGADRIVE_6;
	if (contains_ci(core_path, "mesen-s") ||
	    contains_ci(core_path, "mesen_s") ||
	    contains_ci(core_path, "snes9x") ||
	    contains_ci(core_path, "bsnes"))             return INPUT_PROFILE_SNES;
	if (contains_ci(core_path, "mgba") ||
	    contains_ci(core_path, "vba") ||
	    contains_ci(core_path, "gpsp"))              return INPUT_PROFILE_GBA;
	if (contains_ci(core_path, "gambatte") ||
	    contains_ci(core_path, "sameboy") ||
	    contains_ci(core_path, "tgbdual"))           return INPUT_PROFILE_GB;
	if (contains_ci(core_path, "fceumm") ||
	    contains_ci(core_path, "nestopia") ||
	    contains_ci(core_path, "quicknes") ||
	    contains_ci(core_path, "mesen"))             return INPUT_PROFILE_NES;
	if (contains_ci(core_path, "supergrafx") ||
	    contains_ci(core_path, "_pce") ||
	    contains_ci(core_path, "pce_"))              return INPUT_PROFILE_PC_ENGINE;
	if (contains_ci(core_path, "fbneo") ||
	    contains_ci(core_path, "fbalpha") ||
	    contains_ci(core_path, "mame") ||
	    contains_ci(core_path, "neocd"))             return INPUT_PROFILE_NEOGEO;
	if (contains_ci(core_path, "saturn") ||
	    contains_ci(core_path, "kronos") ||
	    contains_ci(core_path, "yabause"))           return INPUT_PROFILE_SATURN;
	if (contains_ci(core_path, "mupen64plus") ||
	    contains_ci(core_path, "parallel_n64") ||
	    contains_ci(core_path, "parallel-n64"))      return INPUT_PROFILE_N64;
	if (contains_ci(core_path, "psx") ||
	    contains_ci(core_path, "pcsx") ||
	    contains_ci(core_path, "duckstation") ||
	    contains_ci(core_path, "swanstation"))       return INPUT_PROFILE_PSX;
	if (contains_ci(core_path, "flycast") ||
	    contains_ci(core_path, "reicast"))           return INPUT_PROFILE_DREAMCAST;
	if (contains_ci(core_path, "ppsspp"))            return INPUT_PROFILE_PSP;

	return INPUT_PROFILE_GENERIC;
}

const char *input_profile_display_name(input_profile_t profile)
{
	switch (profile) {
		case INPUT_PROFILE_MASTER_SYSTEM: return "Master System / Game Gear";
		case INPUT_PROFILE_MEGADRIVE_3:   return lang_get(STR_PROFILE_MEGADRIVE_3);
		case INPUT_PROFILE_MEGADRIVE_6:   return lang_get(STR_PROFILE_MEGADRIVE_6);
		case INPUT_PROFILE_SNES:          return "Super Nintendo";
		case INPUT_PROFILE_NES:           return "Nintendo (NES)";
		case INPUT_PROFILE_GB:            return "Game Boy";
		case INPUT_PROFILE_GBA:           return "Game Boy Advance";
		case INPUT_PROFILE_PC_ENGINE:     return "PC Engine / TG-16";
		case INPUT_PROFILE_NEOGEO:        return "Neo Geo / Arcade";
		case INPUT_PROFILE_JAGUAR:        return "Atari Jaguar";
		case INPUT_PROFILE_SATURN:        return "Sega Saturn";
		case INPUT_PROFILE_N64:           return "Nintendo 64";
		case INPUT_PROFILE_PSX:           return "PlayStation";
		case INPUT_PROFILE_DREAMCAST:     return "Sega Dreamcast";
		case INPUT_PROFILE_PSP:           return "PSP";
		case INPUT_PROFILE_GENERIC:
		default:                          return lang_get(STR_PROFILE_GENERIC);
	}
}

const char *input_profile_fallback_button_name(input_profile_t profile,
                                               int retro_id)
{
	static char name_buf[32];
	switch (profile) {
	case INPUT_PROFILE_MASTER_SYSTEM:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "C";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Select";
			default: return NULL;
		}
	case INPUT_PROFILE_MEGADRIVE_6:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "C";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "Y";
			case RETRO_DEVICE_ID_JOYPAD_L:      return "X";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "Z";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Mode";
			default: return NULL;
		}
	case INPUT_PROFILE_MEGADRIVE_3:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "C";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Mode";
			default: return NULL;
		}
	case INPUT_PROFILE_SNES:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "Y";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "X";
			case RETRO_DEVICE_ID_JOYPAD_L:      return "L";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "R";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Select";
			default: return NULL;
		}
	case INPUT_PROFILE_NES:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "Turbo B";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "Turbo A";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Select";
			default: return NULL;
		}
	case INPUT_PROFILE_GB:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Select";
			default: return NULL;
		}
	case INPUT_PROFILE_GBA:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_L:      return "L";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "R";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Select";
			default: return NULL;
		}
	case INPUT_PROFILE_PC_ENGINE:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_A:      return "I";
			case RETRO_DEVICE_ID_JOYPAD_B:      return "II";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "III";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "IV";
			case RETRO_DEVICE_ID_JOYPAD_L:      return "V";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "VI";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Run";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Select";
			default: return NULL;
		}
	case INPUT_PROFILE_NEOGEO:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "C";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "D";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return lang_get(STR_BTN_COIN);
			default: return NULL;
		}
	case INPUT_PROFILE_JAGUAR:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "C";
			case RETRO_DEVICE_ID_JOYPAD_X:      snprintf(name_buf, sizeof(name_buf), lang_get(STR_BTN_NUMPAD), 0); return name_buf;
			case RETRO_DEVICE_ID_JOYPAD_L:      snprintf(name_buf, sizeof(name_buf), lang_get(STR_BTN_NUMPAD), 1); return name_buf;
			case RETRO_DEVICE_ID_JOYPAD_R:      snprintf(name_buf, sizeof(name_buf), lang_get(STR_BTN_NUMPAD), 2); return name_buf;
			case RETRO_DEVICE_ID_JOYPAD_L2:     snprintf(name_buf, sizeof(name_buf), lang_get(STR_BTN_NUMPAD), 3); return name_buf;
			case RETRO_DEVICE_ID_JOYPAD_R2:     snprintf(name_buf, sizeof(name_buf), lang_get(STR_BTN_NUMPAD), 4); return name_buf;
			case RETRO_DEVICE_ID_JOYPAD_L3:     snprintf(name_buf, sizeof(name_buf), lang_get(STR_BTN_NUMPAD), 5); return name_buf;
			case RETRO_DEVICE_ID_JOYPAD_R3:     snprintf(name_buf, sizeof(name_buf), lang_get(STR_BTN_NUMPAD), 6); return name_buf;
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Option";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Pause";
			default: return NULL;
		}
	case INPUT_PROFILE_SATURN:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "X";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "Y";
			case RETRO_DEVICE_ID_JOYPAD_L:      return "Z";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "C";
			case RETRO_DEVICE_ID_JOYPAD_L2:     return "L";
			case RETRO_DEVICE_ID_JOYPAD_R2:     return "R";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Mode";
			default: return NULL;
		}
	case INPUT_PROFILE_N64:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_A:      return lang_get(STR_BTN_C_RIGHT);
			case RETRO_DEVICE_ID_JOYPAD_X:      return lang_get(STR_BTN_C_UP);
			case RETRO_DEVICE_ID_JOYPAD_L:      return "L";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "R";
			case RETRO_DEVICE_ID_JOYPAD_L2:     return "Z";
			case RETRO_DEVICE_ID_JOYPAD_R2:     return lang_get(STR_BTN_C_MODE);
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			default: return NULL;
		}
	case INPUT_PROFILE_PSX:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "Cross";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "Circle";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "Square";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "Triangle";
			case RETRO_DEVICE_ID_JOYPAD_L:      return "L1";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "R1";
			case RETRO_DEVICE_ID_JOYPAD_L2:     return "L2";
			case RETRO_DEVICE_ID_JOYPAD_R2:     return "R2";
			case RETRO_DEVICE_ID_JOYPAD_L3:     return "L3";
			case RETRO_DEVICE_ID_JOYPAD_R3:     return "R3";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Select";
			default: return NULL;
		}
	case INPUT_PROFILE_DREAMCAST:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "A";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "B";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "X";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "Y";
			case RETRO_DEVICE_ID_JOYPAD_L:      return "L";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "R";
			case RETRO_DEVICE_ID_JOYPAD_L2:     return lang_get(STR_BTN_L_LIGHT);
			case RETRO_DEVICE_ID_JOYPAD_R2:     return lang_get(STR_BTN_R_LIGHT);
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			default: return NULL;
		}
	case INPUT_PROFILE_PSP:
		switch (retro_id) {
			case RETRO_DEVICE_ID_JOYPAD_B:      return "Cross";
			case RETRO_DEVICE_ID_JOYPAD_A:      return "Circle";
			case RETRO_DEVICE_ID_JOYPAD_Y:      return "Square";
			case RETRO_DEVICE_ID_JOYPAD_X:      return "Triangle";
			case RETRO_DEVICE_ID_JOYPAD_L:      return "L";
			case RETRO_DEVICE_ID_JOYPAD_R:      return "R";
			case RETRO_DEVICE_ID_JOYPAD_START:  return "Start";
			case RETRO_DEVICE_ID_JOYPAD_SELECT: return "Select";
			default: return NULL;
		}
	case INPUT_PROFILE_GENERIC:
	default:
		return NULL;
	}
}

/* ─────────────────── game_profile (manual) ─────────────────── */

static bool  gp_active = false;
static char  gp_name[128] = {0};
static char *gp_buttons[GP_MAX_PORTS][GP_MAX_BUTTONS] = {{0}};

static void game_profile_clear(void)
{
	for (int p = 0; p < GP_MAX_PORTS; p++)
		for (int b = 0; b < GP_MAX_BUTTONS; b++) {
			free(gp_buttons[p][b]);
			gp_buttons[p][b] = NULL;
		}
	gp_name[0] = '\0';
	gp_active = false;
}

static int game_profile_ini_handler(void *user, const char *section,
                                    const char *name, const char *value)
{
	(void)user;

	if (strcmp(section, "meta") == 0) {
		if (strcmp(name, "name") == 0) {
			strncpy(gp_name, value, sizeof(gp_name) - 1);
			gp_name[sizeof(gp_name) - 1] = '\0';
		}
		return 1;
	}

	int port = -1;
	if (sscanf(section, "port%d", &port) != 1 || port < 0 || port >= GP_MAX_PORTS)
		return 1;

	int btn = -1;
	if (sscanf(name, "button_%d", &btn) != 1 || btn < 0 || btn >= GP_MAX_BUTTONS)
		return 1;

	free(gp_buttons[port][btn]);
	gp_buttons[port][btn] = strdup(value);
	return 1;
}

void game_profile_init(const char *value)
{
	char path[512];

	game_profile_clear();

	if (!value || !value[0] ||
	    strcmp(value, "none") == 0 || strcmp(value, "auto") == 0)
		return;

	snprintf(path, sizeof(path), "./profiles/%s.ini", value);

	if (ini_parse(path, game_profile_ini_handler, NULL) != 0) {
		printf("game_profile nao encontrado: %s\n", path);
		game_profile_clear();
		return;
	}

	if (gp_name[0] == '\0') {
		strncpy(gp_name, value, sizeof(gp_name) - 1);
		gp_name[sizeof(gp_name) - 1] = '\0';
	}

	gp_active = true;
	printf("game_profile carregado: %s\n", path);
}

bool game_profile_active(void)
{
	return gp_active;
}

const char *game_profile_name(void)
{
	return gp_active ? gp_name : NULL;
}

const char *game_profile_button_name(int port, int retro_id)
{
	if (!gp_active || port < 0 || port >= GP_MAX_PORTS ||
	    retro_id < 0 || retro_id >= GP_MAX_BUTTONS)
		return NULL;
	return gp_buttons[port][retro_id];
}
