#include <stdbool.h>

typedef struct
{
	const char* title;
    const char* core;
    const char* rom;
    const char* shader;
    const char* filter;
    int swap_interval;
    bool fullscreen;
    int window_width;
    int window_height;
    float aspect_ratio;
    bool hide_cursor;
    bool map_analog_to_dpad;
    unsigned port0;
    unsigned port1;
    unsigned port2;
    unsigned port3;
    const char* language;
    int ff_speed;
    int ff_button;
    int volume;
    bool mute;
    const char* input_profile;   /* perfil de fallback de controle (auto/generic/...) */
    const char* game_profile;    /* perfil manual por jogo (none/<nome>) */
} config;

void cfg_defaults(config* c);
int cfg_handler(void* user, const char* section, const char* name, const char* value);
int cfg_save(const char* path, const config* c);
