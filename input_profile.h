/*
 * input_profile.h — Perfis de controle (fallback) e perfis por jogo.
 *
 * Os controles são mostrados do ponto de vista do CONSOLE EMULADO. A fonte
 * principal dos nomes é o core (input_descriptors.h). Este módulo cobre os
 * outros níveis de prioridade:
 *   1. game_profile  — nomes manuais por jogo (nunca inventado automaticamente).
 *   2. (input descriptors do core)
 *   3. input_profile — fallback por console quando o core não informa nada.
 *   4. (nome genérico RetroPad)
 *
 * Tabelas validadas em controles_por_core.md.
 */

#ifndef INPUT_PROFILE_H
#define INPUT_PROFILE_H

#include <stdbool.h>

typedef enum {
	INPUT_PROFILE_GENERIC = 0,
	INPUT_PROFILE_MASTER_SYSTEM,
	INPUT_PROFILE_MEGADRIVE_3,
	INPUT_PROFILE_MEGADRIVE_6,
	INPUT_PROFILE_SNES,
	INPUT_PROFILE_NES,
	INPUT_PROFILE_GB,
	INPUT_PROFILE_GBA,
	INPUT_PROFILE_PC_ENGINE,
	INPUT_PROFILE_NEOGEO,
	INPUT_PROFILE_JAGUAR,
	INPUT_PROFILE_SATURN,
	INPUT_PROFILE_N64,
	INPUT_PROFILE_PSX,
	INPUT_PROFILE_DREAMCAST,
	INPUT_PROFILE_PSP,
	INPUT_PROFILE_COUNT
} input_profile_t;

/* ─── input_profile (fallback) ─── */

/* Converte o valor textual do config.ini em perfil.
 * Retorna -1 para NULL/vazio/"auto" (peça detecção automática). */
int input_profile_parse(const char *s);

/* Decide o perfil ativo. Em "auto", detecta pelo nome do core. */
input_profile_t input_profile_detect(const char *core_path,
                                     const char *rom_path,
                                     const char *config_value);

/* Nome amigável do console (ex.: "Atari Jaguar"). */
const char *input_profile_display_name(input_profile_t profile);

/* Nome de fallback de um botão RetroPad neste perfil, ou NULL se o perfil não
 * tiver nome específico para esse id (usar genérico então). */
const char *input_profile_fallback_button_name(input_profile_t profile,
                                               int retro_id);

/* ─── game_profile (manual, por jogo, opcional) ─── */

/* Carrega ./profiles/<value>.ini. NULL/""/"none"/"auto" desativa. */
void game_profile_init(const char *value);

/* true se há um perfil de jogo manual carregado. */
bool game_profile_active(void);

/* Rótulo do perfil de jogo (de [meta] name, ou o próprio id). */
const char *game_profile_name(void);

/* Nome manual da AÇÃO de um botão (prioridade máxima), ou NULL. */
const char *game_profile_button_name(int port, int retro_id);

#endif /* INPUT_PROFILE_H */
