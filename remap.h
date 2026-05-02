#ifndef REMAP_H
#define REMAP_H

#include <stdbool.h>

#define REMAP_MAX_BUTTONS 16
#define REMAP_MAX_PORTS   4

/* Inicializa o sistema de remapeamento para a ROM atual.
 * Tenta carregar ./saves/<rom_basename>.remap.ini automaticamente. */
void remap_init(const char *rom_path);

/* Salva a configuração de remap atual em ./saves/<rom_basename>.remap.ini */
void remap_save(void);

/* Carrega a configuração de remap de ./saves/<rom_basename>.remap.ini */
void remap_load(void);

/* Define o mapeamento: botão retro 'retro_id' na porta 'port' → botão GLFW 'glfw_button' */
void remap_set(int port, unsigned retro_id, unsigned glfw_button);

/* Retorna o botão GLFW mapeado para o botão retro 'retro_id' na porta 'port' */
unsigned remap_get(int port, unsigned retro_id);

/* Restaura o mapeamento padrão (Xbox 360) para uma porta específica */
void remap_reset_defaults(int port);

/* Restaura o mapeamento padrão para todas as portas */
void remap_reset_all_defaults(void);

/* Retorna o RETRO_DEVICE_ID_JOYPAD_* que o gatilho L2/R2 aciona (padrão: L2/R2) */
unsigned remap_get_trigger_l2_target(int port);
unsigned remap_get_trigger_r2_target(int port);

/* Modo analógico → D-pad por porta: -1=herda global, 0=off, 1=esq, 2=dir, 3=ambos */
int remap_get_analog_dpad_mode(int port);

#endif /* REMAP_H */
