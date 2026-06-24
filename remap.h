#ifndef REMAP_H
#define REMAP_H

#include <stdbool.h>

#define REMAP_MAX_BUTTONS 16
#define REMAP_MAX_PORTS   4

/* Códigos "físicos" que um botão emulado pode acionar.
 *
 * Modelo unificado: cada botão RetroPad (0..15) é mapeado para UM código físico,
 * que pode ser:
 *   - 0 .. GLFW_GAMEPAD_BUTTON_LAST : um botão digital do gamepad GLFW
 *   - REMAP_PHYS_LT / REMAP_PHYS_RT : os gatilhos analógicos (eixos) como botão
 *   - REMAP_PHYS_NONE               : não mapeado (sem entrada física)
 *
 * Isso conserta o problema antigo em que L2/R2 (gatilhos) não podiam ser
 * remapeados nem usados como destino. */
#define REMAP_PHYS_LT    0x100u
#define REMAP_PHYS_RT    0x101u
#define REMAP_PHYS_NONE  0x1FFu

/* Inicializa o remap para a ROM atual (carrega ./saves/<rom>.remap.ini). */
void remap_init(const char *rom_path);

/* Salva / carrega o remap por jogo. */
void remap_save(void);
void remap_load(void);

/* Define / lê o código físico do botão retro 'retro_id' na porta 'port'. */
void remap_set(int port, unsigned retro_id, unsigned phys_code);
unsigned remap_get(int port, unsigned retro_id);

/* Restaura o padrão para uma porta / todas as portas. */
void remap_reset_defaults(int port);
void remap_reset_all_defaults(void);

/* Modo analógico → D-pad por porta: -1=herda global, 0=off, 1=esq, 2=dir, 3=ambos */
int remap_get_analog_dpad_mode(int port);

#endif /* REMAP_H */
