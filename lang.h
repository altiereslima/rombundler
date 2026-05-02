/*
 * lang.h — Sistema de internacionalização do ROMBundler.
 *
 * Suporta 8 idiomas: PT-BR, EN, ES, FR, IT, DE, ZH, HI.
 * Detecção automática pelo idioma do sistema operacional.
 */

#ifndef LANG_H
#define LANG_H

/* ─── Idiomas suportados ─── */
typedef enum {
	LANG_PT = 0,   /* Português (BR) */
	LANG_EN,       /* English */
	LANG_ES,       /* Español */
	LANG_FR,       /* Français */
	LANG_IT,       /* Italiano */
	LANG_DE,       /* Deutsch */
	LANG_ZH,       /* 中文 (Mandarim / Chinês Simplificado) */
	LANG_HI,       /* हिन्दी */
	LANG_COUNT
} language_t;

/* ─── IDs de strings traduzíveis ─── */
typedef enum {
	/* Menu Principal */
	STR_SETTINGS = 0,
	STR_RESUME,
	STR_VIDEO_SETTINGS,
	STR_INPUT_REMAP,
	STR_CORE_OPTIONS,
	STR_EXIT_GAME,
	STR_LANGUAGE,
	STR_HINT_MAIN,          /* "A/Enter: Select   B/Esc: Close" */

	/* Video Settings */
	STR_VIDEO_HEADER,
	STR_FULLSCREEN,
	STR_SHADER,
	STR_FILTER,
	STR_BACK,
	STR_ON,
	STR_OFF,
	STR_HINT_VIDEO,

	/* Audio Settings */
	STR_AUDIO_SETTINGS,
	STR_AUDIO_HEADER,
	STR_VOLUME,
	STR_MUTE,
	STR_HINT_AUDIO,

	/* Input / Remap */
	STR_INPUT_HEADER,
	STR_CONTROLLER_PORT,    /* "Controller Port: %d" */
	STR_PRESS_BUTTON,       /* "[PRESS A BUTTON...]" */
	STR_RESET_DEFAULTS,
	STR_HINT_INPUT,

	/* Botões (retro) */
	STR_BTN_A,
	STR_BTN_B,
	STR_BTN_X,
	STR_BTN_Y,
	STR_BTN_UP,
	STR_BTN_DOWN,
	STR_BTN_LEFT,
	STR_BTN_RIGHT,
	STR_BTN_START,
	STR_BTN_SELECT,
	STR_BTN_L,
	STR_BTN_R,
	STR_BTN_L2,
	STR_BTN_R2,
	STR_BTN_L3,
	STR_BTN_R3,

	/* Core Options */
	STR_CORE_OPT_HEADER,
	STR_NO_CORE_OPTIONS,
	STR_HINT_CORE_OPTIONS,

	/* Confirm Exit */
	STR_EXIT_CONFIRM,       /* "EXIT GAME?" */
	STR_YES_QUIT,
	STR_NO_CANCEL,

	/* Idiomas (display names) */
	STR_LANG_AUTO,
	STR_LANG_PT,
	STR_LANG_EN,
	STR_LANG_ES,
	STR_LANG_FR,
	STR_LANG_IT,
	STR_LANG_DE,
	STR_LANG_ZH,
	STR_LANG_HI,

	/* Aspect Ratio */
	STR_ASPECT_HEADER,
	STR_ASPECT_RATIO,
	STR_ASPECT_CORE,
	STR_ASPECT_STRETCH,
	STR_ASPECT_4_3,
	STR_ASPECT_16_9,
	STR_ASPECT_CUSTOM,
	STR_ASPECT_ZOOM,
	STR_ASPECT_CUSTOM_EDIT,
	STR_ASPECT_RESET,
	STR_HINT_ASPECT,
	STR_HINT_ASPECT_EDIT,

	STR_COUNT               /* Total de strings */
} string_id;

/* ─── API ─── */

/* Inicializa o sistema de idioma (auto-detecção se lang == "auto") */
void lang_init(const char *lang_setting);

/* Retorna a string traduzida para o idioma ativo */
const char *lang_get(string_id id);

/* Define o idioma manualmente */
void lang_set(language_t lang);

/* Retorna o idioma ativo */
language_t lang_current(void);

/* Retorna o código do idioma (ex: "pt", "en", "es", "fr", "it", "de", "zh", "hi") */
const char *lang_code(language_t lang);

/* Retorna o nome de exibição do idioma (ex: "Português", "English") */
const char *lang_display_name(language_t lang);

/* Cicla para o próximo idioma */
void lang_cycle(int dir);

/* Mapeia o idioma atual para RETRO_LANGUAGE_* (para uso com cores libretro) */
unsigned lang_to_retro(void);

#endif /* LANG_H */
