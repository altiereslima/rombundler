#ifndef VIDEO_H
#define VIDEO_H

#include "libretro.h"
#include <stdbool.h>

void create_window(int width, int height);
void video_configure(const struct retro_game_geometry *geom);
bool video_set_system_av_info(const struct retro_system_av_info *av);
bool video_set_pixel_format(unsigned format);
void video_set_geometry(const struct retro_game_geometry *geom);
void video_set_hw(struct retro_hw_render_callback);
bool video_hw_context_supported(unsigned context_type);
void video_should_close(int v);
void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch);
uintptr_t video_get_current_framebuffer();
retro_proc_address_t video_get_proc_address(const char *sym);
const struct retro_hw_render_interface *video_get_hw_render_interface(void);
bool video_set_hw_render_context_negotiation_interface(
	const struct retro_hw_render_context_negotiation_interface *iface);
bool video_get_hw_render_context_negotiation_interface_support(
	struct retro_hw_render_context_negotiation_interface *iface);
void video_render();
void video_deinit();
bool video_uses_vulkan(void);
bool video_menu_supported(void);

/* Alterna entre tela cheia e janela */
void video_toggle_fullscreen(void);

/* Retorna true se está em tela cheia */
bool video_is_fullscreen(void);
void video_apply_cursor_mode(void);

void video_reload_shader(void);
void video_reload_filter(void);
void video_set_fast_forward(bool enabled);

/* GL state management for HW-rendering cores (N64, PSP, etc.) */
void video_gl_unbind(void);   /* Call before core_run() */
void video_gl_rebind(void);   /* Call after core_run()  */

#endif /* VIDEO_H */
