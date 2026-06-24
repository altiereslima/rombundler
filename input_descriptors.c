/*
 * input_descriptors.c — Cópia segura dos input descriptors do core Libretro.
 * Veja input_descriptors.h.
 */

#include <string.h>

#include "input_descriptors.h"
#include "libretro.h"

static struct input_desc_entry desc_table[INPUT_DESC_MAX];
static unsigned desc_count = 0;

void input_desc_clear(void)
{
	desc_count = 0;
	memset(desc_table, 0, sizeof(desc_table));
}

void input_desc_set(const void *descs)
{
	const struct retro_input_descriptor *d =
		(const struct retro_input_descriptor *)descs;

	input_desc_clear();

	if (!d)
		return;

	while (d->description != NULL && desc_count < INPUT_DESC_MAX) {
		struct input_desc_entry *e = &desc_table[desc_count];

		e->port   = d->port;
		e->device = d->device;
		e->index  = d->index;
		e->id     = d->id;
		strncpy(e->description, d->description, INPUT_DESC_DESC_LEN - 1);
		e->description[INPUT_DESC_DESC_LEN - 1] = '\0';

		desc_count++;
		d++;
	}
}

unsigned input_desc_count(void)
{
	return desc_count;
}

bool input_desc_available(void)
{
	return desc_count > 0;
}

const char *input_desc_lookup(unsigned port, unsigned device,
                              unsigned index, unsigned id)
{
	for (unsigned i = 0; i < desc_count; i++) {
		const struct input_desc_entry *e = &desc_table[i];
		if (e->port == port && e->device == device &&
		    e->index == index && e->id == id &&
		    e->description[0] != '\0')
			return e->description;
	}
	return NULL;
}

const char *input_desc_joypad(unsigned port, unsigned id)
{
	return input_desc_lookup(port, RETRO_DEVICE_JOYPAD, 0, id);
}

const struct input_desc_entry *input_desc_get(unsigned i)
{
	if (i >= desc_count)
		return NULL;
	return &desc_table[i];
}
