#ifndef VIDEO_VULKAN_H
#define VIDEO_VULKAN_H

#include <stdbool.h>
#include <stddef.h>

#include "libretro.h"

struct retro_hw_render_interface;
struct retro_hw_render_context_negotiation_interface;
struct retro_game_geometry;

typedef struct GLFWwindow GLFWwindow;

bool video_vulkan_supported(void);
bool video_vulkan_init(GLFWwindow *window,
	const struct retro_game_geometry *geom,
	const struct retro_hw_render_callback *hw);
void video_vulkan_deinit(void);

void video_vulkan_set_geometry(const struct retro_game_geometry *geom);
void video_vulkan_begin_frame(void);
void video_vulkan_refresh(const void *data, unsigned width, unsigned height, size_t pitch);
void video_vulkan_render(void);
void video_vulkan_render_menu(void);
void video_vulkan_mark_swapchain_dirty(void);
void video_vulkan_set_fast_forward(bool enabled);

const struct retro_hw_render_interface *video_vulkan_get_hw_render_interface(void);
bool video_vulkan_set_negotiation_interface(
	const struct retro_hw_render_context_negotiation_interface *iface);
bool video_vulkan_get_negotiation_interface_support(
	struct retro_hw_render_context_negotiation_interface *iface);

#endif /* VIDEO_VULKAN_H */
