#ifndef SRM_H
#define SRM_H

/* Define o caminho do save por jogo (./saves/<rom>.srm).
 * Chamar antes de srm_load/srm_save. Sem isso, usa ./save.srm (compatibilidade). */
void srm_init(const char *rom_path);

void srm_save(void);
void srm_load(void);

#endif /* SRM_H */
