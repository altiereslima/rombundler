/*
 * srm.c — Persistência da SRAM (save da bateria) por jogo.
 *
 * Correções:
 *  - Abre em modo BINÁRIO ("wb"/"rb"): no Windows o modo texto corrompe bytes
 *    0x0A da SRAM.
 *  - Caminho por jogo em ./saves/<rom>.srm, em vez de um ./save.srm fixo
 *    compartilhado entre todos os jogos.
 */

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

#include "libretro.h"
#include "core.h"
#include "srm.h"

static char srm_filepath[512] = "./save.srm";

void srm_init(const char *rom_path)
{
	if (!rom_path || !rom_path[0]) {
		strcpy(srm_filepath, "./save.srm");
		return;
	}

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
	snprintf(srm_filepath, sizeof(srm_filepath), "./saves/%s.srm", name);
}

void srm_save(void)
{
	size_t size = core_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	void *data = core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!size || !data)
		return;

	FILE *f = fopen(srm_filepath, "wb");
	if (!f)
		return;

	fwrite(data, 1, size, f);
	fclose(f);
}

void srm_load(void)
{
	size_t size = core_get_memory_size(RETRO_MEMORY_SAVE_RAM);
	void *data = core_get_memory_data(RETRO_MEMORY_SAVE_RAM);
	if (!size || !data)
		return;

	FILE *f = fopen(srm_filepath, "rb");
	if (!f)
		return;

	fread(data, 1, size, f);
	fclose(f);
}
