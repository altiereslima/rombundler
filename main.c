/*
 * main.c — Ponto de entrada do ROMBundler.
 *
 * Loop principal com suporte a menu overlay:
 * - Quando o menu está ativo, a emulação pausa e o menu é renderizado.
 * - Atalhos: ALT+ENTER, F1, BACK+START (processados em input.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "mappings.h"
#include "config.h"
#include "options.h"
#include "core.h"
#include "audio.h"
#include "video.h"
#include "video_vulkan.h"
#include "input.h"
#include "ini.h"
#include "srm.h"
#include "utils.h"
#include "font.h"
#include "menu.h"
#include "remap.h"
#include "lang.h"
#include "aspect.h"

extern GLFWwindow *window;
config g_cfg;

static void persist_runtime_config(void)
{
	if (window) {
		g_cfg.fullscreen = video_is_fullscreen();
		if (!g_cfg.fullscreen)
			glfwGetWindowSize(window, &g_cfg.window_width, &g_cfg.window_height);
	}

	g_cfg.language = lang_code(lang_current());
	g_cfg.volume = audio_get_volume();
	g_cfg.mute = audio_get_mute();

	if (cfg_save("./config.ini", &g_cfg) < 0) {
		log_printf("app", "failed to save config.ini");
		return;
	}

	log_printf("app",
		"config.ini saved fullscreen=%d size=%dx%d lang=%s volume=%d mute=%d",
		g_cfg.fullscreen,
		g_cfg.window_width,
		g_cfg.window_height,
		g_cfg.language ? g_cfg.language : "(null)",
		g_cfg.volume,
		g_cfg.mute);
}

static void error_cb(int error, const char* description)
{
	(void)error;
	fprintf(stderr, "Erro: %s\n", description);
	log_printf("glfw", "error %d: %s", error, description ? description : "(null)");
}

void joystick_callback(int jid, int event)
{
	if (event == GLFW_CONNECTED)
		printf("Conectado: %s %s\n", glfwGetGamepadName(jid), glfwGetJoystickGUID(jid));
	else if (event == GLFW_DISCONNECTED)
		printf("Controle %d desconectado\n", jid);
}

static bool core_path_contains(const char *needle)
{
	return g_cfg.core && needle && strstr(g_cfg.core, needle) != NULL;
}

static bool str_in_list(const char *value, const char *const *values, int count)
{
	if (!value || !values || count <= 0)
		return false;

	for (int i = 0; i < count; i++) {
		if (values[i] && strcmp(value, values[i]) == 0)
			return true;
	}

	return false;
}

static bool core_self_manages_hw_binding(void)
{
	const char *rdp_plugin = NULL;

	if (!core_path_contains("mupen64plus_next") &&
		!core_path_contains("mupen64plus"))
		return false;

	if (get_option("mupen64plus-rdp-plugin", &rdp_plugin) &&
		rdp_plugin && strcmp(rdp_plugin, "gliden64") == 0)
		return true;

	return false;
}

static void log_effective_option(const char *key)
{
	const char *value = NULL;

	if (get_option(key, &value) && value)
		log_printf("app", "effective option %s=%s", key, value);
	else
		log_printf("app", "effective option %s=(unavailable)", key);
}

static void apply_core_fallback_overrides(bool options_loaded)
{
	static const char *const mupen_vulkan_rdp_candidates[] = {
		"parallel"
	};
	static const char *const mupen_software_rdp_candidates[] = {
		"angrylion-rdp-plus",
		"angrylion-plus",
		"angrylion"
	};
	static const char *const mupen_gl_rdp_candidates[] = {
		"gliden64"
	};
	static const char *const mupen_vulkan_rsp_candidates[] = {
		"parallel",
		"hle"
	};
	static const char *const mupen_software_rsp_candidates[] = {
		"cxd4",
		"hle"
	};
	static const char *const mupen_gl_rsp_candidates[] = {
		"hle",
		"cxd4"
	};
	static const char *const mupen_parallel_upscaling_candidates[] = {
		"4x",
		"2x",
		"1x",
		"8x"
	};
	static const char *const mupen_parallel_downscaling_candidates[] = {
		"1/4",
		"1/2",
		"1/8",
		"disable"
	};
	static const char *const mupen_gl_upscaling_candidates[] = {
		"4",
		"3",
		"2",
		"1",
		"8",
		"7",
		"6",
		"5",
		"0"
	};
	const int vulkan_rdp_count =
		(int)(sizeof(mupen_vulkan_rdp_candidates) / sizeof(mupen_vulkan_rdp_candidates[0]));
	const int software_rdp_count =
		(int)(sizeof(mupen_software_rdp_candidates) / sizeof(mupen_software_rdp_candidates[0]));
	const int gl_rdp_count =
		(int)(sizeof(mupen_gl_rdp_candidates) / sizeof(mupen_gl_rdp_candidates[0]));
	const int vulkan_rsp_count =
		(int)(sizeof(mupen_vulkan_rsp_candidates) / sizeof(mupen_vulkan_rsp_candidates[0]));
	const int software_rsp_count =
		(int)(sizeof(mupen_software_rsp_candidates) / sizeof(mupen_software_rsp_candidates[0]));
	const int gl_rsp_count =
		(int)(sizeof(mupen_gl_rsp_candidates) / sizeof(mupen_gl_rsp_candidates[0]));
	const int parallel_upscaling_count =
		(int)(sizeof(mupen_parallel_upscaling_candidates) / sizeof(mupen_parallel_upscaling_candidates[0]));
	const int parallel_downscaling_count =
		(int)(sizeof(mupen_parallel_downscaling_candidates) / sizeof(mupen_parallel_downscaling_candidates[0]));
	const int gl_upscaling_count =
		(int)(sizeof(mupen_gl_upscaling_candidates) / sizeof(mupen_gl_upscaling_candidates[0]));
	bool prefer_vulkan = video_hw_context_supported(RETRO_HW_CONTEXT_VULKAN);
	const char *selected_rdp = NULL;
	const char *selected_rsp = NULL;
	const char *current_rdp = NULL;
	const char *current_rsp = NULL;
	const char *current_upscaling = NULL;
	const char *current_downscaling = NULL;
	bool rdp_ok = false;
	bool rsp_ok = false;
	bool fb_ok = false;
	bool keep_loaded_rdp = false;
	bool keep_loaded_rsp = false;
	bool sanitized_loaded_options = false;
	bool preset_applied = false;

	if (!core_path_contains("mupen64plus_next"))
		return;

	if (prefer_vulkan) {
		prefer_vulkan = false;
		log_printf("app", "forcing OpenGL for mupen64plus_next (Vulkan disabled)");
	}

	if (options_loaded &&
		get_option("mupen64plus-rdp-plugin", &current_rdp) &&
		current_rdp &&
		opt_value_allowed("mupen64plus-rdp-plugin", current_rdp) &&
		((prefer_vulkan &&
		 str_in_list(current_rdp, mupen_vulkan_rdp_candidates, vulkan_rdp_count)) ||
		 (!prefer_vulkan &&
		 (str_in_list(current_rdp, mupen_gl_rdp_candidates, gl_rdp_count) ||
		  str_in_list(current_rdp, mupen_software_rdp_candidates, software_rdp_count))))) {
		selected_rdp = current_rdp;
		keep_loaded_rdp = true;
	}

	if (!keep_loaded_rdp) {
		if (prefer_vulkan) {
			rdp_ok = opt_override_first_available("mupen64plus-rdp-plugin",
				mupen_vulkan_rdp_candidates,
				vulkan_rdp_count,
				&selected_rdp);
		}

		if (!rdp_ok) {
			rdp_ok = opt_override_first_available("mupen64plus-rdp-plugin",
				mupen_gl_rdp_candidates,
				gl_rdp_count,
				&selected_rdp);
		}

		if (!rdp_ok) {
			rdp_ok = opt_override_first_available("mupen64plus-rdp-plugin",
				mupen_software_rdp_candidates,
				software_rdp_count,
				&selected_rdp);
		}

		sanitized_loaded_options = options_loaded;
	}

	if (selected_rdp && strcmp(selected_rdp, "parallel") == 0) {
		if (options_loaded &&
			get_option("mupen64plus-rsp-plugin", &current_rsp) &&
			current_rsp &&
			str_in_list(current_rsp, mupen_vulkan_rsp_candidates, vulkan_rsp_count) &&
			strcmp(current_rsp, "parallel") == 0) {
			selected_rsp = current_rsp;
			keep_loaded_rsp = true;
		}

		if (!keep_loaded_rsp) {
			rsp_ok = opt_override_first_available("mupen64plus-rsp-plugin",
				mupen_vulkan_rsp_candidates,
				vulkan_rsp_count,
				&selected_rsp);
		}
	} else if (selected_rdp &&
		(strstr(selected_rdp, "angrylion") != NULL)) {
		if (options_loaded &&
			get_option("mupen64plus-rsp-plugin", &current_rsp) &&
			current_rsp &&
			str_in_list(current_rsp, mupen_software_rsp_candidates, software_rsp_count)) {
			selected_rsp = current_rsp;
			keep_loaded_rsp = true;
		}

		if (!keep_loaded_rsp) {
			rsp_ok = opt_override_first_available("mupen64plus-rsp-plugin",
				mupen_software_rsp_candidates,
				software_rsp_count,
				&selected_rsp);
		}
	} else {
		if (options_loaded &&
			get_option("mupen64plus-rsp-plugin", &current_rsp) &&
			current_rsp &&
			str_in_list(current_rsp, mupen_gl_rsp_candidates, gl_rsp_count)) {
			selected_rsp = current_rsp;
			keep_loaded_rsp = true;
		}

		if (!keep_loaded_rsp) {
			rsp_ok = opt_override_first_available("mupen64plus-rsp-plugin",
				mupen_gl_rsp_candidates,
				gl_rsp_count,
				&selected_rsp);
		}
	}

	if (!keep_loaded_rsp && options_loaded)
		sanitized_loaded_options = true;

	if (selected_rdp && strcmp(selected_rdp, "parallel") == 0) {
		if (!options_loaded ||
			!get_option("mupen64plus-parallel-rdp-upscaling", &current_upscaling) ||
			!current_upscaling ||
			strcmp(current_upscaling, "4x") != 0) {
			preset_applied |= opt_override_first_available("mupen64plus-parallel-rdp-upscaling",
				mupen_parallel_upscaling_candidates,
				parallel_upscaling_count,
				NULL);
		}

		if (!options_loaded ||
			!get_option("mupen64plus-parallel-rdp-downscaling", &current_downscaling) ||
			!current_downscaling ||
			strcmp(current_downscaling, "1/4") != 0) {
			preset_applied |= opt_override_first_available("mupen64plus-parallel-rdp-downscaling",
				mupen_parallel_downscaling_candidates,
				parallel_downscaling_count,
				NULL);
		}
	} else if (selected_rdp && strcmp(selected_rdp, "gliden64") == 0) {
		if (!options_loaded ||
			!get_option("mupen64plus-EnableNativeResFactor", &current_upscaling) ||
			!current_upscaling ||
			strcmp(current_upscaling, "4") != 0) {
			preset_applied |= opt_override_first_available("mupen64plus-EnableNativeResFactor",
				mupen_gl_upscaling_candidates,
				gl_upscaling_count,
				NULL);
		}
	}

	if ((!options_loaded || sanitized_loaded_options) &&
		selected_rdp && strcmp(selected_rdp, "gliden64") == 0)
		fb_ok = opt_override("mupen64plus-EnableFBEmulation", "False");

	if (sanitized_loaded_options) {
		log_printf("app",
			"sanitized unsupported mupen options rdp=%s rsp=%s fb=%s",
			selected_rdp ? selected_rdp : "unchanged",
			selected_rsp ? selected_rsp : "unchanged",
			fb_ok ? "False" : "unchanged");
	} else if (!options_loaded) {
		log_printf("app",
			"applied adaptive mupen fallback rdp=%s rsp=%s fb=%s",
			rdp_ok && selected_rdp ? selected_rdp : "unchanged",
			rsp_ok && selected_rsp ? selected_rsp : "unchanged",
			fb_ok ? "False" : "unchanged");
	} else {
		log_printf("app",
			"kept loaded mupen options rdp=%s rsp=%s fb=%s",
			selected_rdp ? selected_rdp : "unchanged",
			selected_rsp ? selected_rsp : "unchanged",
			"unchanged");
	}

	if (preset_applied) {
		log_printf("app",
			"applied mupen render preset backend=%s",
			selected_rdp ? selected_rdp : "unchanged");
	}

	log_effective_option("mupen64plus-rdp-plugin");
	log_effective_option("mupen64plus-rsp-plugin");
	log_effective_option("mupen64plus-EnableFBEmulation");
	if (selected_rdp && strcmp(selected_rdp, "parallel") == 0) {
		log_effective_option("mupen64plus-parallel-rdp-upscaling");
		log_effective_option("mupen64plus-parallel-rdp-downscaling");
	} else if (selected_rdp && strcmp(selected_rdp, "gliden64") == 0) {
		log_effective_option("mupen64plus-EnableNativeResFactor");
	}
}

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;
	bool options_loaded = false;
	bool menu_supported = false;
	const char *force_menu_env = NULL;
	int config_parse_result = 0;

	log_init("./rombundler.log");
	log_printf("app", "ROMBundler startup");

	cfg_defaults(&g_cfg);
	config_parse_result = ini_parse("./config.ini", cfg_handler, &g_cfg);
	if (config_parse_result < 0)
		log_printf("app", "config.ini not found, using defaults");
	else if (config_parse_result > 0)
		log_printf("app", "config.ini parse warning at line %d, using parsed values", config_parse_result);
	else
		log_printf("app", "config.ini parsed");

	log_printf("app", "config loaded: title='%s' core='%s' rom='%s'",
		g_cfg.title ? g_cfg.title : "(null)",
		g_cfg.core ? g_cfg.core : "(null)",
		g_cfg.rom ? g_cfg.rom : "(null)");

	/* Inicializa o sistema de idiomas (auto-detecta se config diz "auto") */
	lang_init(g_cfg.language);
	audio_set_volume(g_cfg.volume);
	audio_set_mute(g_cfg.mute);

	glfwSetErrorCallback(error_cb);

	if (!glfwInit())
		die("Falha ao inicializar GLFW");

	log_printf("app", "GLFW initialized");

	if (!glfwUpdateGamepadMappings(mappings))
		die("Falha ao carregar mapeamentos de controle");
	else
		printf("Mapeamentos de controle atualizados\n");

	glfwSetJoystickCallback(joystick_callback);

	core_load(g_cfg.core);
	log_printf("app", "loading options.ini");
	options_loaded = (opt_load("./options.ini") >= 0);
	if (!options_loaded)
		log_printf("app", "options.ini not found or unreadable, continuing with core defaults");
	else
		log_printf("app", "options.ini parsed");
	apply_core_fallback_overrides(options_loaded);

	core_load_game(g_cfg.rom);
	menu_supported = video_menu_supported();

	/* Inicializa o sistema de fonte e menu. */
	if (menu_supported) {
		if (!video_uses_vulkan()) {
			font_init();
			menu_init();
		} else {
			menu_init();
			log_printf("app", "menu overlay enabled on Vulkan path (SW rendering)");
		}

		force_menu_env = getenv("ROMBUNDLER_FORCE_MENU");
		if (force_menu_env && strcmp(force_menu_env, "1") == 0) {
			menu_toggle();
			log_printf("app", "forcing menu active at startup via ROMBUNDLER_FORCE_MENU=1");
		}
	} else {
		log_printf("app", "menu overlay disabled on Vulkan path");
	}

	/* Inicializa o remapeamento e aspecto por jogo */
	remap_init(g_cfg.rom);
	aspect_init(g_cfg.rom);

	srm_load();

	if (!video_uses_vulkan())
		glfwSwapInterval(g_cfg.swap_interval);

	bool self_managed_hw_binding = core_self_manages_hw_binding();
	if (self_managed_hw_binding)
		log_printf("app", "using self-managed HW rebind path for core '%s' (frontend prebind enabled)",
			g_cfg.core ? g_cfg.core : "(null)");

	unsigned frame = 0;
	bool last_fast_forward = false;
	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();
		input_poll();

		if (menu_supported && menu_is_active()) {
			/* Menu ativo: processa entrada do menu, não roda o core */
			bool should_quit = menu_input();
			if (should_quit) break;

			/* Renderiza o overlay do menu */
			if (video_uses_vulkan()) {
				video_vulkan_render_menu();
			} else {
				video_render();
				menu_render();
			}
		} else {
			bool fast_forward = input_is_fast_forward();

			if (fast_forward != last_fast_forward) {
				log_printf("app", "fast-forward active=%d ratio=%d vulkan=%d",
					fast_forward ? 1 : 0,
					g_cfg.ff_speed,
					video_uses_vulkan() ? 1 : 0);
				video_set_fast_forward(fast_forward);
				last_fast_forward = fast_forward;
			}

			if (fast_forward) {
				if (!video_uses_vulkan())
					glfwSwapInterval(0);
				audio_set_nonblocking(true);
			} else {
				if (!video_uses_vulkan())
					glfwSwapInterval(g_cfg.swap_interval);
				audio_set_nonblocking(false);
			}

			video_gl_unbind();
			core_run();
			if (!self_managed_hw_binding)
				video_gl_rebind();
			video_render();
		}

		if (!video_uses_vulkan())
			glfwSwapBuffers(window);
		frame++;
		if (frame % 600 == 0)
			srm_save();
	}

	/* Salva tudo antes de sair */
	persist_runtime_config();
	srm_save();
	remap_save();
	opt_save("./options.ini");

	if (menu_supported) {
		menu_deinit();
		font_deinit();
	}
	video_deinit();
	core_unload();
	audio_deinit();

	glfwTerminate();
	return 0;
}
