#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "ini.h"
#include "libretro.h"

void cfg_defaults(config *c)
{
	c->title = "ROMBundler";
	c->shader = "default";
	c->filter = "nearest";
	c->fullscreen = true;
	c->window_width = 800;
	c->window_height = 600;
	c->aspect_ratio = 4.0 / 3.0;
	c->swap_interval = 1;
	c->hide_cursor = false;
	c->map_analog_to_dpad = true;
	c->port0 = RETRO_DEVICE_NONE;
	c->port1 = RETRO_DEVICE_NONE;
	c->port2 = RETRO_DEVICE_NONE;
	c->port3 = RETRO_DEVICE_NONE;
	c->language = "auto";
	c->ff_speed = 4;
	c->ff_button = -1;
	c->volume = 100;
	c->mute = false;
}

int cfg_handler(void* user, const char* section, const char* name, const char* value)
{
	config* c = (config*)user;

	#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
	if (MATCH("", "title"))
		c->title = strdup(value);
	else if (MATCH("", "core"))
		c->core = strdup(value);
	else if (MATCH("", "rom"))
		c->rom = strdup(value);
	else if (MATCH("", "shader"))
		c->shader = strdup(value);
	else if (MATCH("", "filter"))
		c->filter = strdup(value);
	else if (MATCH("", "swap_interval"))
		c->swap_interval = atoi(value);
	else if (MATCH("", "window_width"))
		c->window_width = atoi(value);
	else if (MATCH("", "window_height"))
		c->window_height = atoi(value);
	else if (MATCH("", "aspect_ratio"))
		c->aspect_ratio = atof(value);
	else if (MATCH("", "fullscreen"))
		c->fullscreen = strcmp(value, "true") == 0;
	else if (MATCH("", "hide_cursor"))
		c->hide_cursor = strcmp(value, "true") == 0;
	else if (MATCH("", "map_analog_to_dpad"))
		c->map_analog_to_dpad = strcmp(value, "true") == 0;
	else if (MATCH("", "port0"))
		c->port0 = atoi(value);
	else if (MATCH("", "port1"))
		c->port1 = atoi(value);
	else if (MATCH("", "port2"))
		c->port2 = atoi(value);
	else if (MATCH("", "port3"))
		c->port3 = atoi(value);
	else if (MATCH("", "language"))
		c->language = strdup(value);
	else if (MATCH("", "ff_speed"))
		c->ff_speed = atoi(value);
	else if (MATCH("", "ff_button"))
		c->ff_button = atoi(value);
	else if (MATCH("", "volume"))
		c->volume = atoi(value);
	else if (MATCH("", "mute"))
		c->mute = strcmp(value, "true") == 0;
	else
		return 0;
	return 1;
}

int cfg_save(const char* path, const config* c)
{
	FILE *f = NULL;

	if (!path || !c)
		return -1;

	f = fopen(path, "w");
	if (!f)
		return -1;

	if (c->title)    fprintf(f, "title = %s\n", c->title);
	if (c->core)     fprintf(f, "core = %s\n", c->core);
	if (c->rom)      fprintf(f, "rom = %s\n", c->rom);
	if (c->shader)   fprintf(f, "shader = %s\n", c->shader);
	if (c->filter)   fprintf(f, "filter = %s\n", c->filter);
	if (c->language) fprintf(f, "language = %s\n", c->language);

	fprintf(f, "swap_interval = %d\n", c->swap_interval);
	fprintf(f, "fullscreen = %s\n", c->fullscreen ? "true" : "false");
	fprintf(f, "window_width = %d\n", c->window_width);
	fprintf(f, "window_height = %d\n", c->window_height);
	fprintf(f, "aspect_ratio = %.6f\n", c->aspect_ratio);
	fprintf(f, "hide_cursor = %s\n", c->hide_cursor ? "true" : "false");
	fprintf(f, "map_analog_to_dpad = %s\n", c->map_analog_to_dpad ? "true" : "false");
	fprintf(f, "port0 = %u\n", c->port0);
	fprintf(f, "port1 = %u\n", c->port1);
	fprintf(f, "port2 = %u\n", c->port2);
	fprintf(f, "port3 = %u\n", c->port3);
	fprintf(f, "ff_speed = %d\n", c->ff_speed);
	fprintf(f, "ff_button = %d\n", c->ff_button);
	fprintf(f, "volume = %d\n", c->volume);
	fprintf(f, "mute = %s\n", c->mute ? "true" : "false");

	fclose(f);
	return 0;
}
