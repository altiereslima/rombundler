/*
 * input_descriptors.h — Cópia segura dos input descriptors do core Libretro.
 *
 * Um core Libretro pode informar, via RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
 * o NOME REAL de cada botão do console emulado (ex.: Genesis Plus GX informa
 * "A"/"B"/"C"/"X"/"Y"/"Z"/"Start"/"Mode"; virtualjaguar informa "A"/"B"/"C"/
 * "Pause"/"Option"/teclas do keypad; dosbox_pure reescreve dinamicamente).
 *
 * O ponteiro entregue pelo core NÃO é garantido válido após a chamada, então
 * copiamos tudo. Esses nomes são a fonte PRINCIPAL dos rótulos no remap.
 */

#ifndef INPUT_DESCRIPTORS_H
#define INPUT_DESCRIPTORS_H

#include <stdbool.h>

#define INPUT_DESC_MAX        256
#define INPUT_DESC_DESC_LEN   64

/* Uma entrada copiada de retro_input_descriptor (sem depender de libretro.h). */
struct input_desc_entry {
	unsigned port;
	unsigned device;
	unsigned index;
	unsigned id;
	char     description[INPUT_DESC_DESC_LEN];
};

/* Substitui a tabela interna por uma cópia segura do array do core.
 * 'descs' aponta para um array de retro_input_descriptor terminado por uma
 * entrada com description == NULL (void* para manter o header independente de
 * libretro.h). NULL apenas limpa. Pode ser chamada várias vezes — a tabela é
 * sempre recriada do zero (importante para cores dinâmicos como dosbox_pure). */
void input_desc_set(const void *descs);

/* Esvazia a tabela (ex.: ao descarregar o core). */
void input_desc_clear(void);

/* Quantos descritores estão armazenados. */
unsigned input_desc_count(void);

/* true se o core forneceu pelo menos um descritor utilizável. */
bool input_desc_available(void);

/* Procura a descrição para port/device/index/id exatos. NULL se não houver. */
const char *input_desc_lookup(unsigned port, unsigned device,
                              unsigned index, unsigned id);

/* Atalho para botão de joypad (device = RETRO_DEVICE_JOYPAD, index = 0). */
const char *input_desc_joypad(unsigned port, unsigned id);

/* Acesso só-leitura para a tela de depuração. NULL fora do intervalo. */
const struct input_desc_entry *input_desc_get(unsigned i);

#endif /* INPUT_DESCRIPTORS_H */
