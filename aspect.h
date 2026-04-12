/*
 * aspect.h — Sistema de proporção de tela por jogo.
 *
 * Modos: Core Default, Stretch, 4:3, 16:9, Custom.
 * Modo Custom permite ajuste de posição/tamanho via controle.
 * Configuração salva por ROM em subpasta do executável.
 */

#ifndef ASPECT_H
#define ASPECT_H

typedef enum {
	ASPECT_CORE,    /* Proporção definida pelo núcleo */
	ASPECT_STRETCH, /* Esticar para caber na janela */
	ASPECT_4_3,     /* Forçar 4:3 */
	ASPECT_16_9,    /* Forçar 16:9 */
	ASPECT_CUSTOM,  /* Personalizado (editável) */
	ASPECT_COUNT
} aspect_mode_t;

/* Viewport calculado */
typedef struct {
	int x, y, w, h;
} aspect_viewport_t;

/* Offsets customizados (persistidos por jogo) */
typedef struct {
	int off_x, off_y;   /* Deslocamento em pixels */
	int adj_w, adj_h;   /* Ajuste de largura/altura */
} aspect_custom_t;

/* Inicializa o sistema com o nome da ROM (para salvar/carregar por jogo) */
void aspect_init(const char *rom_path);

/* Salva configuração para o jogo atual */
void aspect_save(void);

/* Retorna o modo atual */
aspect_mode_t aspect_get_mode(void);

/* Define o modo */
void aspect_set_mode(aspect_mode_t mode);

/* Cicla o modo (dir=+1 ou -1) */
void aspect_cycle(int dir);

/* Nome do modo atual para exibição */
const char *aspect_mode_name(aspect_mode_t mode);

/* Calcula o viewport final dados:
 * - win_w, win_h: tamanho da janela
 * - core_w, core_h: resolução do núcleo
 * - core_aspect: aspect ratio do núcleo (0 = auto) */
aspect_viewport_t aspect_calc(int win_w, int win_h, int core_w, int core_h, float core_aspect);

/* Custom adjustments */
aspect_custom_t *aspect_get_custom(void);
void aspect_custom_reset(void);

#endif
