/*
 * lang.c — Sistema de internacionalização do ROMBundler.
 *
 * Contém as tabelas de tradução para PT-BR, EN, ES, FR, IT, DE
 * e a lógica de detecção automática de idioma do sistema.
 */

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "lang.h"
#include "libretro.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static language_t current_lang = LANG_PT;

#define LANG_NAME_ZH "\xE4\xB8\xAD\xE6\x96\x87"
#define LANG_NAME_HI "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80"

/* ─────────────────── Tabelas de Tradução ─────────────────── */

static const char *strings_pt[STR_COUNT] = {
	/* Menu Principal */
	[STR_SETTINGS]        = "CONFIGURA\xC3\x87\xC3\x95""ES",
	[STR_RESUME]          = "Continuar",
	[STR_VIDEO_SETTINGS]  = "V\xC3\xAD""deo",
	[STR_AUDIO_SETTINGS]  = "\xC3\x81udio",
	[STR_INPUT_REMAP]     = "Controles / Remapear",
	[STR_CORE_OPTIONS]    = "Op\xC3\xA7\xC3\xB5""es do N\xC3\xBA""cleo",
	[STR_EXIT_GAME]       = "Sair do Jogo",
	[STR_LANGUAGE]        = "Idioma",
	[STR_HINT_MAIN]       = "A/Enter: Selecionar   B/Esc: Fechar",

	/* Video Settings */
	[STR_VIDEO_HEADER]    = "CONFIGURA\xC3\x87\xC3\x95""ES DE V\xC3\x8D""DEO",
	[STR_FULLSCREEN]      = "Tela Cheia",
	[STR_SHADER]          = "Shader",
	[STR_FILTER]          = "Filtro",
	[STR_BACK]            = "Voltar",
	[STR_ON]              = "Ligado",
	[STR_OFF]             = "Desligado",
	[STR_HINT_VIDEO]      = "Esq/Dir: Alterar   A/Enter: Alternar   B/Esc: Voltar",

	/* Audio Settings */
	[STR_AUDIO_HEADER]    = "CONFIGURA\xC3\x87\xC3\x95""ES DE \xC3\x81UDIO",
	[STR_VOLUME]          = "Volume",
	[STR_MUTE]            = "Mudo",
	[STR_HINT_AUDIO]      = "Esq/Dir: Mudar Volume   A/Enter: Alternar Mudo   B/Esc: Voltar",

	/* Input / Remap */
	[STR_INPUT_HEADER]    = "CONTROLES / REMAPEAR",
	[STR_CONTROLLER_PORT] = "Porta do Controle",
	[STR_PRESS_BUTTON]    = "[APERTE UM BOT\xC3\x83O...]",
	[STR_RESET_DEFAULTS]  = "Restaurar Padr\xC3\xB5""es",
	[STR_HINT_INPUT]      = "A/Enter: Remapear   L/R: Trocar Porta   B/Esc: Voltar",

	/* Botões */
	[STR_BTN_A]      = "A",      [STR_BTN_B]     = "B",
	[STR_BTN_X]      = "X",      [STR_BTN_Y]     = "Y",
	[STR_BTN_UP]     = "CIMA",   [STR_BTN_DOWN]  = "BAIXO",
	[STR_BTN_LEFT]   = "ESQ",    [STR_BTN_RIGHT] = "DIR",
	[STR_BTN_START]  = "START",  [STR_BTN_SELECT]= "SELECT",
	[STR_BTN_L]      = "L",     [STR_BTN_R]      = "R",
	[STR_BTN_L2]     = "L2",    [STR_BTN_R2]     = "R2",
	[STR_BTN_L3]     = "L3",    [STR_BTN_R3]     = "R3",

	/* Core Options */
	[STR_CORE_OPT_HEADER] = "OP\xC3\x87\xC3\x95""ES DO N\xC3\x9A""CLEO",
	[STR_NO_CORE_OPTIONS] = "Nenhuma op\xC3\xA7\xC3\xA3o dispon\xC3\xADvel.",
	[STR_HINT_CORE_OPTIONS] = "Esq/Dir: Alterar   B/Esc: Voltar",

	/* Confirm Exit */
	[STR_EXIT_CONFIRM]    = "SAIR DO JOGO?",
	[STR_YES_QUIT]        = "Sim - Sair",
	[STR_NO_CANCEL]       = "N\xC3\xA3o - Cancelar",

	/* Language names */
	[STR_LANG_AUTO] = "Autom\xC3\xA1tico",
	[STR_LANG_PT]   = "Portugu\xC3\xAAs",
	[STR_LANG_EN]   = "English",
	[STR_LANG_ES]   = "Espa\xC3\xB1ol",
	[STR_LANG_FR]   = "Fran\xC3\xA7""ais",
	[STR_LANG_IT]   = "Italiano",
	[STR_LANG_DE]   = "Deutsch",
	[STR_LANG_ZH]   = LANG_NAME_ZH,
	[STR_LANG_HI]   = LANG_NAME_HI,

	/* Aspect Ratio */
	[STR_ASPECT_HEADER]     = "PROPOR\xC3\x87\xC3\x83O DE TELA",
	[STR_ASPECT_RATIO]      = "Propor\xC3\xA7\xC3\xA3o",
	[STR_ASPECT_CORE]       = "Padr\xC3\xA3o do N\xC3\xBA""cleo",
	[STR_ASPECT_STRETCH]    = "Esticar",
	[STR_ASPECT_4_3]        = "4:3",
	[STR_ASPECT_16_9]       = "16:9",
	[STR_ASPECT_CUSTOM]     = "Personalizado",
	[STR_ASPECT_CUSTOM_EDIT]= "Ajustar Posi\xC3\xA7\xC3\xA3o/Tamanho",
	[STR_ASPECT_RESET]      = "Restaurar Padr\xC3\xA3o",
	[STR_HINT_ASPECT]       = "Esq/Dir: Alterar   A: Selecionar   B/Esc: Voltar",
	[STR_HINT_ASPECT_EDIT]  = "D-Pad: Mover   LB/RB: Largura   LT/RT: Altura   A: Salvar   B: Cancelar",
};

static const char *strings_en[STR_COUNT] = {
	[STR_SETTINGS]        = "SETTINGS",
	[STR_RESUME]          = "Resume",
	[STR_VIDEO_SETTINGS]  = "Video",
	[STR_AUDIO_SETTINGS]  = "Audio",
	[STR_INPUT_REMAP]     = "Input / Remap",
	[STR_CORE_OPTIONS]    = "Core Options",
	[STR_EXIT_GAME]       = "Exit Game",
	[STR_LANGUAGE]        = "Language",
	[STR_HINT_MAIN]       = "A/Enter: Select   B/Esc: Close",

	[STR_VIDEO_HEADER]    = "VIDEO SETTINGS",
	[STR_FULLSCREEN]      = "Fullscreen",
	[STR_SHADER]          = "Shader",
	[STR_FILTER]          = "Filter",
	[STR_BACK]            = "Back",
	[STR_ON]              = "ON",
	[STR_OFF]             = "OFF",
	[STR_HINT_VIDEO]      = "Left/Right: Change   A/Enter: Toggle   B/Esc: Back",

	[STR_AUDIO_HEADER]    = "AUDIO SETTINGS",
	[STR_VOLUME]          = "Volume",
	[STR_MUTE]            = "Mute",
	[STR_HINT_AUDIO]      = "Left/Right: Change Volume   A/Enter: Toggle Mute   B/Esc: Back",

	[STR_INPUT_HEADER]    = "INPUT / REMAP",
	[STR_CONTROLLER_PORT] = "Controller Port",
	[STR_PRESS_BUTTON]    = "[PRESS A BUTTON...]",
	[STR_RESET_DEFAULTS]  = "Reset Defaults",
	[STR_HINT_INPUT]      = "A/Enter: Remap   L/R: Change Port   B/Esc: Back",

	[STR_BTN_A]      = "A",     [STR_BTN_B]     = "B",
	[STR_BTN_X]      = "X",     [STR_BTN_Y]     = "Y",
	[STR_BTN_UP]     = "UP",    [STR_BTN_DOWN]  = "DOWN",
	[STR_BTN_LEFT]   = "LEFT",  [STR_BTN_RIGHT] = "RIGHT",
	[STR_BTN_START]  = "START", [STR_BTN_SELECT]= "SELECT",
	[STR_BTN_L]      = "L",    [STR_BTN_R]      = "R",
	[STR_BTN_L2]     = "L2",   [STR_BTN_R2]     = "R2",
	[STR_BTN_L3]     = "L3",   [STR_BTN_R3]     = "R3",

	[STR_CORE_OPT_HEADER] = "CORE OPTIONS",
	[STR_NO_CORE_OPTIONS] = "No core options available.",
	[STR_HINT_CORE_OPTIONS] = "Left/Right: Change   B/Esc: Back",

	[STR_EXIT_CONFIRM]    = "EXIT GAME?",
	[STR_YES_QUIT]        = "Yes - Quit",
	[STR_NO_CANCEL]       = "No - Cancel",

	[STR_LANG_AUTO] = "Automatic",
	[STR_LANG_PT]   = "Portugu\xC3\xAAs",
	[STR_LANG_EN]   = "English",
	[STR_LANG_ES]   = "Espa\xC3\xB1ol",
	[STR_LANG_FR]   = "Fran\xC3\xA7""ais",
	[STR_LANG_IT]   = "Italiano",
	[STR_LANG_DE]   = "Deutsch",
	[STR_LANG_ZH]   = LANG_NAME_ZH,
	[STR_LANG_HI]   = LANG_NAME_HI,

	[STR_ASPECT_HEADER]     = "ASPECT RATIO",
	[STR_ASPECT_RATIO]      = "Aspect Ratio",
	[STR_ASPECT_CORE]       = "Core Default",
	[STR_ASPECT_STRETCH]    = "Stretch",
	[STR_ASPECT_4_3]        = "4:3",
	[STR_ASPECT_16_9]       = "16:9",
	[STR_ASPECT_CUSTOM]     = "Custom",
	[STR_ASPECT_CUSTOM_EDIT]= "Adjust Position/Size",
	[STR_ASPECT_RESET]      = "Reset to Default",
	[STR_HINT_ASPECT]       = "Left/Right: Change   A: Select   B/Esc: Back",
	[STR_HINT_ASPECT_EDIT]  = "D-Pad: Move   LB/RB: Width   LT/RT: Height   A: Save   B: Cancel",
};

static const char *strings_es[STR_COUNT] = {
	[STR_SETTINGS]        = "CONFIGURACI\xC3\x93N",
	[STR_RESUME]          = "Continuar",
	[STR_VIDEO_SETTINGS]  = "V\xC3\xAD""deo",
	[STR_AUDIO_SETTINGS]  = "Audio",
	[STR_INPUT_REMAP]     = "Controles / Reasignar",
	[STR_CORE_OPTIONS]    = "Opciones del N\xC3\xBA""cleo",
	[STR_EXIT_GAME]       = "Salir del Juego",
	[STR_LANGUAGE]        = "Idioma",
	[STR_HINT_MAIN]       = "A/Enter: Seleccionar   B/Esc: Cerrar",

	[STR_VIDEO_HEADER]    = "CONFIGURACI\xC3\x93N DE V\xC3\x8D""DEO",
	[STR_FULLSCREEN]      = "Pantalla Completa",
	[STR_SHADER]          = "Shader",
	[STR_FILTER]          = "Filtro",
	[STR_BACK]            = "Volver",
	[STR_ON]              = "Activado",
	[STR_OFF]             = "Desactivado",
	[STR_HINT_VIDEO]      = "Izq/Der: Cambiar   A/Enter: Alternar   B/Esc: Volver",

	[STR_AUDIO_HEADER]    = "CONFIGURACI\xC3\x93N DE AUDIO",
	[STR_VOLUME]          = "Volumen",
	[STR_MUTE]            = "Silencio",
	[STR_HINT_AUDIO]      = "Izq/Der: Cambiar Volumen   A/Enter: Alternar Silencio   B/Esc: Volver",

	[STR_INPUT_HEADER]    = "CONTROLES / REASIGNAR",
	[STR_CONTROLLER_PORT] = "Puerto del Control",
	[STR_PRESS_BUTTON]    = "[PULSE UN BOT\xC3\x93N...]",
	[STR_RESET_DEFAULTS]  = "Restaurar Valores",
	[STR_HINT_INPUT]      = "A/Enter: Reasignar   L/R: Cambiar Puerto   B/Esc: Volver",

	[STR_BTN_A]      = "A",       [STR_BTN_B]     = "B",
	[STR_BTN_X]      = "X",       [STR_BTN_Y]     = "Y",
	[STR_BTN_UP]     = "ARRIBA",  [STR_BTN_DOWN]  = "ABAJO",
	[STR_BTN_LEFT]   = "IZQ",     [STR_BTN_RIGHT] = "DER",
	[STR_BTN_START]  = "START",   [STR_BTN_SELECT]= "SELECT",
	[STR_BTN_L]      = "L",      [STR_BTN_R]      = "R",
	[STR_BTN_L2]     = "L2",     [STR_BTN_R2]     = "R2",
	[STR_BTN_L3]     = "L3",     [STR_BTN_R3]     = "R3",

	[STR_CORE_OPT_HEADER] = "OPCIONES DEL N\xC3\x9A""CLEO",
	[STR_NO_CORE_OPTIONS] = "No hay opciones disponibles.",
	[STR_HINT_CORE_OPTIONS] = "Izq/Der: Cambiar   B/Esc: Volver",

	[STR_EXIT_CONFIRM]    = "\xC2\xBFSALIR DEL JUEGO?",
	[STR_YES_QUIT]        = "S\xC3\xAD - Salir",
	[STR_NO_CANCEL]       = "No - Cancelar",

	[STR_LANG_AUTO] = "Autom\xC3\xA1tico",
	[STR_LANG_PT]   = "Portugu\xC3\xAAs",
	[STR_LANG_EN]   = "English",
	[STR_LANG_ES]   = "Espa\xC3\xB1ol",
	[STR_LANG_FR]   = "Fran\xC3\xA7""ais",
	[STR_LANG_IT]   = "Italiano",
	[STR_LANG_DE]   = "Deutsch",
	[STR_LANG_ZH]   = LANG_NAME_ZH,
	[STR_LANG_HI]   = LANG_NAME_HI,

	[STR_ASPECT_HEADER]     = "PROPORCI\xC3\x93N DE PANTALLA",
	[STR_ASPECT_RATIO]      = "Proporci\xC3\xB3n",
	[STR_ASPECT_CORE]       = "Predeterminado del N\xC3\xBA""cleo",
	[STR_ASPECT_STRETCH]    = "Estirar",
	[STR_ASPECT_4_3]        = "4:3",
	[STR_ASPECT_16_9]       = "16:9",
	[STR_ASPECT_CUSTOM]     = "Personalizado",
	[STR_ASPECT_CUSTOM_EDIT]= "Ajustar Posici\xC3\xB3n/Tama\xC3\xB1o",
	[STR_ASPECT_RESET]      = "Restaurar Valores",
	[STR_HINT_ASPECT]       = "Izq/Der: Cambiar   A: Seleccionar   B/Esc: Volver",
	[STR_HINT_ASPECT_EDIT]  = "D-Pad: Mover   LB/RB: Ancho   LT/RT: Alto   A: Guardar   B: Cancelar",
};

static const char *strings_fr[STR_COUNT] = {
	[STR_SETTINGS]        = "PARAM\xC3\x88TRES",
	[STR_RESUME]          = "Reprendre",
	[STR_VIDEO_SETTINGS]  = "Vid\xC3\xA9o",
	[STR_AUDIO_SETTINGS]  = "Audio",
	[STR_INPUT_REMAP]     = "Contr\xC3\xB4les / R\xC3\xA9""assigner",
	[STR_CORE_OPTIONS]    = "Options du Noyau",
	[STR_EXIT_GAME]       = "Quitter le Jeu",
	[STR_LANGUAGE]        = "Langue",
	[STR_HINT_MAIN]       = "A/Entr\xC3\xA9""e: Choisir   B/\xC3\x89""chap: Fermer",

	[STR_VIDEO_HEADER]    = "PARAM\xC3\x88TRES VID\xC3\x89O",
	[STR_FULLSCREEN]      = "Plein \xC3\x89""cran",
	[STR_SHADER]          = "Shader",
	[STR_FILTER]          = "Filtre",
	[STR_BACK]            = "Retour",
	[STR_ON]              = "Activ\xC3\xA9",
	[STR_OFF]             = "D\xC3\xA9sactiv\xC3\xA9",
	[STR_HINT_VIDEO]      = "Gauche/Droite: Changer   A/Entr\xC3\xA9""e: Basculer   B/\xC3\x89""chap: Retour",

	[STR_AUDIO_HEADER]    = "PARAM\xC3\x88TRES AUDIO",
	[STR_VOLUME]          = "Volume",
	[STR_MUTE]            = "Muet",
	[STR_HINT_AUDIO]      = "Gauche/Droite: Changer Volume   A/Entr\xC3\xA9""e: Basculer Muet   B/\xC3\x89""chap: Retour",

	[STR_INPUT_HEADER]    = "CONTR\xC3\x94LES / R\xC3\x89""ASSIGNER",
	[STR_CONTROLLER_PORT] = "Port Manette",
	[STR_PRESS_BUTTON]    = "[APPUYEZ SUR UN BOUTON...]",
	[STR_RESET_DEFAULTS]  = "Restaurer les Param\xC3\xA8tres",
	[STR_HINT_INPUT]      = "A/Entr\xC3\xA9""e: R\xC3\xA9""assigner   L/R: Changer Port   B/\xC3\x89""chap: Retour",

	[STR_BTN_A]      = "A",      [STR_BTN_B]     = "B",
	[STR_BTN_X]      = "X",      [STR_BTN_Y]     = "Y",
	[STR_BTN_UP]     = "HAUT",   [STR_BTN_DOWN]  = "BAS",
	[STR_BTN_LEFT]   = "GAUCHE", [STR_BTN_RIGHT] = "DROITE",
	[STR_BTN_START]  = "START",  [STR_BTN_SELECT]= "SELECT",
	[STR_BTN_L]      = "L",     [STR_BTN_R]      = "R",
	[STR_BTN_L2]     = "L2",    [STR_BTN_R2]     = "R2",
	[STR_BTN_L3]     = "L3",    [STR_BTN_R3]     = "R3",

	[STR_CORE_OPT_HEADER] = "OPTIONS DU NOYAU",
	[STR_NO_CORE_OPTIONS] = "Aucune option disponible.",
	[STR_HINT_CORE_OPTIONS] = "Gauche/Droite: Changer   B/\xC3\x89""chap: Retour",

	[STR_EXIT_CONFIRM]    = "QUITTER LE JEU?",
	[STR_YES_QUIT]        = "Oui - Quitter",
	[STR_NO_CANCEL]       = "Non - Annuler",

	[STR_LANG_AUTO] = "Automatique",
	[STR_LANG_PT]   = "Portugu\xC3\xAAs",
	[STR_LANG_EN]   = "English",
	[STR_LANG_ES]   = "Espa\xC3\xB1ol",
	[STR_LANG_FR]   = "Fran\xC3\xA7""ais",
	[STR_LANG_IT]   = "Italiano",
	[STR_LANG_DE]   = "Deutsch",
	[STR_LANG_ZH]   = LANG_NAME_ZH,
	[STR_LANG_HI]   = LANG_NAME_HI,

	[STR_ASPECT_HEADER]     = "RAPPORT D'ASPECT",
	[STR_ASPECT_RATIO]      = "Rapport d'aspect",
	[STR_ASPECT_CORE]       = "D\xC3\xA9""faut du Noyau",
	[STR_ASPECT_STRETCH]    = "\xC3\x89tirer",
	[STR_ASPECT_4_3]        = "4:3",
	[STR_ASPECT_16_9]       = "16:9",
	[STR_ASPECT_CUSTOM]     = "Personnalis\xC3\xA9",
	[STR_ASPECT_CUSTOM_EDIT]= "Ajuster Position/Taille",
	[STR_ASPECT_RESET]      = "Restaurer les Param\xC3\xA8tres",
	[STR_HINT_ASPECT]       = "Gauche/Droite: Changer   A: Choisir   B/\xC3\x89""chap: Retour",
	[STR_HINT_ASPECT_EDIT]  = "D-Pad: D\xC3\xA9placer   LB/RB: Largeur   LT/RT: Hauteur   A: Sauver   B: Annuler",
};

static const char *strings_it[STR_COUNT] = {
	[STR_SETTINGS]        = "IMPOSTAZIONI",
	[STR_RESUME]          = "Riprendi",
	[STR_VIDEO_SETTINGS]  = "Video",
	[STR_AUDIO_SETTINGS]  = "Audio",
	[STR_INPUT_REMAP]     = "Controlli / Rimappa",
	[STR_CORE_OPTIONS]    = "Opzioni del Core",
	[STR_EXIT_GAME]       = "Esci dal Gioco",
	[STR_LANGUAGE]        = "Lingua",
	[STR_HINT_MAIN]       = "A/Invio: Seleziona   B/Esc: Chiudi",

	[STR_VIDEO_HEADER]    = "IMPOSTAZIONI VIDEO",
	[STR_FULLSCREEN]      = "Schermo Intero",
	[STR_SHADER]          = "Shader",
	[STR_FILTER]          = "Filtro",
	[STR_BACK]            = "Indietro",
	[STR_ON]              = "Attivo",
	[STR_OFF]             = "Disattivo",
	[STR_HINT_VIDEO]      = "Sin/Des: Cambia   A/Invio: Alterna   B/Esc: Indietro",

	[STR_AUDIO_HEADER]    = "IMPOSTAZIONI AUDIO",
	[STR_VOLUME]          = "Volume",
	[STR_MUTE]            = "Muto",
	[STR_HINT_AUDIO]      = "Sin/Des: Cambia Volume   A/Invio: Alterna Muto   B/Esc: Indietro",

	[STR_INPUT_HEADER]    = "CONTROLLI / RIMAPPA",
	[STR_CONTROLLER_PORT] = "Porta Controller",
	[STR_PRESS_BUTTON]    = "[PREMI UN TASTO...]",
	[STR_RESET_DEFAULTS]  = "Ripristina Predefiniti",
	[STR_HINT_INPUT]      = "A/Invio: Rimappa   L/R: Cambia Porta   B/Esc: Indietro",

	[STR_BTN_A]      = "A",      [STR_BTN_B]     = "B",
	[STR_BTN_X]      = "X",      [STR_BTN_Y]     = "Y",
	[STR_BTN_UP]     = "SU",     [STR_BTN_DOWN]  = "GI\xC3\x99",
	[STR_BTN_LEFT]   = "SIN",    [STR_BTN_RIGHT] = "DES",
	[STR_BTN_START]  = "START",  [STR_BTN_SELECT]= "SELECT",
	[STR_BTN_L]      = "L",     [STR_BTN_R]      = "R",
	[STR_BTN_L2]     = "L2",    [STR_BTN_R2]     = "R2",
	[STR_BTN_L3]     = "L3",    [STR_BTN_R3]     = "R3",

	[STR_CORE_OPT_HEADER] = "OPZIONI DEL CORE",
	[STR_NO_CORE_OPTIONS] = "Nessuna opzione disponibile.",
	[STR_HINT_CORE_OPTIONS] = "Sin/Des: Cambia   B/Esc: Indietro",

	[STR_EXIT_CONFIRM]    = "USCIRE DAL GIOCO?",
	[STR_YES_QUIT]        = "S\xC3\xAC - Esci",
	[STR_NO_CANCEL]       = "No - Annulla",

	[STR_LANG_AUTO] = "Automatico",
	[STR_LANG_PT]   = "Portugu\xC3\xAAs",
	[STR_LANG_EN]   = "English",
	[STR_LANG_ES]   = "Espa\xC3\xB1ol",
	[STR_LANG_FR]   = "Fran\xC3\xA7""ais",
	[STR_LANG_IT]   = "Italiano",
	[STR_LANG_DE]   = "Deutsch",
	[STR_LANG_ZH]   = LANG_NAME_ZH,
	[STR_LANG_HI]   = LANG_NAME_HI,

	[STR_ASPECT_HEADER]     = "RAPPORTO D'ASPETTO",
	[STR_ASPECT_RATIO]      = "Rapporto d'aspetto",
	[STR_ASPECT_CORE]       = "Predefinito del Core",
	[STR_ASPECT_STRETCH]    = "Stirare",
	[STR_ASPECT_4_3]        = "4:3",
	[STR_ASPECT_16_9]       = "16:9",
	[STR_ASPECT_CUSTOM]     = "Personalizzato",
	[STR_ASPECT_CUSTOM_EDIT]= "Regola Posizione/Dimensione",
	[STR_ASPECT_RESET]      = "Ripristina Predefinito",
	[STR_HINT_ASPECT]       = "Sin/Des: Cambia   A: Seleziona   B/Esc: Indietro",
	[STR_HINT_ASPECT_EDIT]  = "D-Pad: Muovi   LB/RB: Larghezza   LT/RT: Altezza   A: Salva   B: Annulla",
};

static const char *strings_de[STR_COUNT] = {
	[STR_SETTINGS]        = "EINSTELLUNGEN",
	[STR_RESUME]          = "Fortsetzen",
	[STR_VIDEO_SETTINGS]  = "Video",
	[STR_AUDIO_SETTINGS]  = "Audio",
	[STR_INPUT_REMAP]     = "Steuerung / Neubelegung",
	[STR_CORE_OPTIONS]    = "Core-Optionen",
	[STR_EXIT_GAME]       = "Spiel Beenden",
	[STR_LANGUAGE]        = "Sprache",
	[STR_HINT_MAIN]       = "A/Enter: Ausw\xC3\xA4hlen   B/Esc: Schlie\xC3\x9F""en",

	[STR_VIDEO_HEADER]    = "VIDEO-EINSTELLUNGEN",
	[STR_FULLSCREEN]      = "Vollbild",
	[STR_SHADER]          = "Shader",
	[STR_FILTER]          = "Filter",
	[STR_BACK]            = "Zur\xC3\xBC""ck",
	[STR_ON]              = "Ein",
	[STR_OFF]             = "Aus",
	[STR_HINT_VIDEO]      = "Links/Rechts: \xC3\x84ndern   A/Enter: Umschalten   B/Esc: Zur\xC3\xBC""ck",

	[STR_AUDIO_HEADER]    = "AUDIO-EINSTELLUNGEN",
	[STR_VOLUME]          = "Lautst\xC3\xA4rke",
	[STR_MUTE]            = "Stumm",
	[STR_HINT_AUDIO]      = "Links/Rechts: Lautst\xC3\xA4rke   A/Enter: Stumm   B/Esc: Zur\xC3\xBC""ck",

	[STR_INPUT_HEADER]    = "STEUERUNG / NEUBELEGUNG",
	[STR_CONTROLLER_PORT] = "Controller-Port",
	[STR_PRESS_BUTTON]    = "[TASTE DR\xC3\x9C""CKEN...]",
	[STR_RESET_DEFAULTS]  = "Standardwerte",
	[STR_HINT_INPUT]      = "A/Enter: Neubelegen   L/R: Port Wechseln   B/Esc: Zur\xC3\xBC""ck",

	[STR_BTN_A]      = "A",       [STR_BTN_B]     = "B",
	[STR_BTN_X]      = "X",       [STR_BTN_Y]     = "Y",
	[STR_BTN_UP]     = "OBEN",    [STR_BTN_DOWN]  = "UNTEN",
	[STR_BTN_LEFT]   = "LINKS",   [STR_BTN_RIGHT] = "RECHTS",
	[STR_BTN_START]  = "START",   [STR_BTN_SELECT]= "SELECT",
	[STR_BTN_L]      = "L",      [STR_BTN_R]      = "R",
	[STR_BTN_L2]     = "L2",     [STR_BTN_R2]     = "R2",
	[STR_BTN_L3]     = "L3",     [STR_BTN_R3]     = "R3",

	[STR_CORE_OPT_HEADER] = "CORE-OPTIONEN",
	[STR_NO_CORE_OPTIONS] = "Keine Core-Optionen verf\xC3\xBC""gbar.",
	[STR_HINT_CORE_OPTIONS] = "Links/Rechts: \xC3\x84ndern   B/Esc: Zur\xC3\xBC""ck",

	[STR_EXIT_CONFIRM]    = "SPIEL BEENDEN?",
	[STR_YES_QUIT]        = "Ja - Beenden",
	[STR_NO_CANCEL]       = "Nein - Abbrechen",

	[STR_LANG_AUTO] = "Automatisch",
	[STR_LANG_PT]   = "Portugu\xC3\xAAs",
	[STR_LANG_EN]   = "English",
	[STR_LANG_ES]   = "Espa\xC3\xB1ol",
	[STR_LANG_FR]   = "Fran\xC3\xA7""ais",
	[STR_LANG_IT]   = "Italiano",
	[STR_LANG_DE]   = "Deutsch",
	[STR_LANG_ZH]   = LANG_NAME_ZH,
	[STR_LANG_HI]   = LANG_NAME_HI,

	[STR_ASPECT_HEADER]     = "SEITENVERH\xC3\x84LTNIS",
	[STR_ASPECT_RATIO]      = "Seitenverh\xC3\xA4ltnis",
	[STR_ASPECT_CORE]       = "Core-Standard",
	[STR_ASPECT_STRETCH]    = "Strecken",
	[STR_ASPECT_4_3]        = "4:3",
	[STR_ASPECT_16_9]       = "16:9",
	[STR_ASPECT_CUSTOM]     = "Benutzerdefiniert",
	[STR_ASPECT_CUSTOM_EDIT]= "Position/Gr\xC3\xB6\xC3\x9F""e Anpassen",
	[STR_ASPECT_RESET]      = "Standard Wiederherstellen",
	[STR_HINT_ASPECT]       = "Links/Rechts: \xC3\x84ndern   A: Ausw\xC3\xA4hlen   B/Esc: Zur\xC3\xBC""ck",
	[STR_HINT_ASPECT_EDIT]  = "D-Pad: Bewegen   LB/RB: Breite   LT/RT: H\xC3\xB6he   A: Speichern   B: Abbrechen",
};

static const char *strings_zh[STR_COUNT] = {
	[STR_SETTINGS]        = "\xE8\xAE\xBE\xE7\xBD\xAE",
	[STR_RESUME]          = "\xE7\xBB\xA7\xE7\xBB\xAD",
	[STR_VIDEO_SETTINGS]  = "\xE8\xA7\x86\xE9\xA2\x91",
	[STR_AUDIO_SETTINGS]  = "\xE9\x9F\xB3\xE9\xA2\x91",
	[STR_INPUT_REMAP]     = "\xE6\x8E\xA7\xE5\x88\xB6\x20\x2F\x20\xE9\x87\x8D\xE6\x98\xA0\xE5\xB0\x84",
	[STR_CORE_OPTIONS]    = "\xE6\xA0\xB8\xE5\xBF\x83\xE9\x80\x89\xE9\xA1\xB9",
	[STR_EXIT_GAME]       = "\xE9\x80\x80\xE5\x87\xBA\xE6\xB8\xB8\xE6\x88\x8F",
	[STR_LANGUAGE]        = "\xE8\xAF\xAD\xE8\xA8\x80",
	[STR_HINT_MAIN]       = "\x41\x2F\x45\x6E\x74\x65\x72\x3A\x20\xE9\x80\x89\xE6\x8B\xA9\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE5\x85\xB3\xE9\x97\xAD",

	[STR_VIDEO_HEADER]    = "\xE8\xA7\x86\xE9\xA2\x91\xE8\xAE\xBE\xE7\xBD\xAE",
	[STR_FULLSCREEN]      = "\xE5\x85\xA8\xE5\xB1\x8F",
	[STR_SHADER]          = "\xE7\x9D\x80\xE8\x89\xB2\xE5\x99\xA8",
	[STR_FILTER]          = "\xE6\xBB\xA4\xE9\x95\x9C",
	[STR_BACK]            = "\xE8\xBF\x94\xE5\x9B\x9E",
	[STR_ON]              = "\xE5\xBC\x80\xE5\x90\xAF",
	[STR_OFF]             = "\xE5\x85\xB3\xE9\x97\xAD",
	[STR_HINT_VIDEO]      = "\xE5\xB7\xA6\x2F\xE5\x8F\xB3\x3A\x20\xE6\x9B\xB4\xE6\x94\xB9\x20\x20\x20\x41\x2F\x45\x6E\x74\x65\x72\x3A\x20\xE5\x88\x87\xE6\x8D\xA2\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE8\xBF\x94\xE5\x9B\x9E",

	[STR_AUDIO_HEADER]    = "\xE9\x9F\xB3\xE9\xA2\x91\xE8\xAE\xBE\xE7\xBD\xAE",
	[STR_VOLUME]          = "\xE9\x9F\xB3\xE9\x87\x8F",
	[STR_MUTE]            = "\xE9\x9D\x99\xE9\x9F\xB3",
	[STR_HINT_AUDIO]      = "\xE5\xB7\xA6\x2F\xE5\x8F\xB3\x3A\x20\xE8\xB0\x83\xE6\x95\xB4\xE9\x9F\xB3\xE9\x87\x8F\x20\x20\x20\x41\x2F\x45\x6E\x74\x65\x72\x3A\x20\xE5\x88\x87\xE6\x8D\xA2\xE9\x9D\x99\xE9\x9F\xB3\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE8\xBF\x94\xE5\x9B\x9E",

	[STR_INPUT_HEADER]    = "\xE6\x8E\xA7\xE5\x88\xB6\x20\x2F\x20\xE9\x87\x8D\xE6\x98\xA0\xE5\xB0\x84",
	[STR_CONTROLLER_PORT] = "\xE6\x8E\xA7\xE5\x88\xB6\xE5\x99\xA8\xE7\xAB\xAF\xE5\x8F\xA3",
	[STR_PRESS_BUTTON]    = "\x5B\xE6\x8C\x89\xE4\xB8\x8B\xE4\xB8\x80\xE4\xB8\xAA\xE6\x8C\x89\xE9\x92\xAE\x2E\x2E\x2E\x5D",
	[STR_RESET_DEFAULTS]  = "\xE6\x81\xA2\xE5\xA4\x8D\xE9\xBB\x98\xE8\xAE\xA4",
	[STR_HINT_INPUT]      = "\x41\x2F\x45\x6E\x74\x65\x72\x3A\x20\xE9\x87\x8D\xE6\x98\xA0\xE5\xB0\x84\x20\x20\x20\x4C\x2F\x52\x3A\x20\xE5\x88\x87\xE6\x8D\xA2\xE7\xAB\xAF\xE5\x8F\xA3\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE8\xBF\x94\xE5\x9B\x9E",

	[STR_BTN_A]      = "A",      [STR_BTN_B]     = "B",
	[STR_BTN_X]      = "X",      [STR_BTN_Y]     = "Y",
	[STR_BTN_UP]     = "\xE4\xB8\x8A",
	[STR_BTN_DOWN]   = "\xE4\xB8\x8B",
	[STR_BTN_LEFT]   = "\xE5\xB7\xA6",
	[STR_BTN_RIGHT]  = "\xE5\x8F\xB3",
	[STR_BTN_START]  = "START",  [STR_BTN_SELECT]= "SELECT",
	[STR_BTN_L]      = "L",      [STR_BTN_R]     = "R",
	[STR_BTN_L2]     = "L2",     [STR_BTN_R2]    = "R2",
	[STR_BTN_L3]     = "L3",     [STR_BTN_R3]    = "R3",

	[STR_CORE_OPT_HEADER] = "\xE6\xA0\xB8\xE5\xBF\x83\xE9\x80\x89\xE9\xA1\xB9",
	[STR_NO_CORE_OPTIONS] = "\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8F\xAF\xE7\x94\xA8\xE7\x9A\x84\xE6\xA0\xB8\xE5\xBF\x83\xE9\x80\x89\xE9\xA1\xB9\xE3\x80\x82",
	[STR_HINT_CORE_OPTIONS] = "\xE5\xB7\xA6\x2F\xE5\x8F\xB3\x3A\x20\xE6\x9B\xB4\xE6\x94\xB9\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE8\xBF\x94\xE5\x9B\x9E",

	[STR_EXIT_CONFIRM]    = "\xE9\x80\x80\xE5\x87\xBA\xE6\xB8\xB8\xE6\x88\x8F\xEF\xBC\x9F",
	[STR_YES_QUIT]        = "\xE6\x98\xAF\x20\x2D\x20\xE9\x80\x80\xE5\x87\xBA",
	[STR_NO_CANCEL]       = "\xE5\x90\xA6\x20\x2D\x20\xE5\x8F\x96\xE6\xB6\x88",

	[STR_LANG_AUTO] = "\xE8\x87\xAA\xE5\x8A\xA8",
	[STR_LANG_PT]   = "Portugu\xC3\xAAs",
	[STR_LANG_EN]   = "English",
	[STR_LANG_ES]   = "Espa\xC3\xB1ol",
	[STR_LANG_FR]   = "Fran\xC3\xA7""ais",
	[STR_LANG_IT]   = "Italiano",
	[STR_LANG_DE]   = "Deutsch",
	[STR_LANG_ZH]   = LANG_NAME_ZH,
	[STR_LANG_HI]   = LANG_NAME_HI,

	[STR_ASPECT_HEADER]     = "\xE5\xB1\x8F\xE5\xB9\x95\xE6\xAF\x94\xE4\xBE\x8B",
	[STR_ASPECT_RATIO]      = "\xE6\xAF\x94\xE4\xBE\x8B",
	[STR_ASPECT_CORE]       = "\xE6\xA0\xB8\xE5\xBF\x83\xE9\xBB\x98\xE8\xAE\xA4",
	[STR_ASPECT_STRETCH]    = "\xE6\x8B\x89\xE4\xBC\xB8",
	[STR_ASPECT_4_3]        = "4:3",
	[STR_ASPECT_16_9]       = "16:9",
	[STR_ASPECT_CUSTOM]     = "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89",
	[STR_ASPECT_CUSTOM_EDIT]= "\xE8\xB0\x83\xE6\x95\xB4\xE4\xBD\x8D\xE7\xBD\xAE\x2F\xE5\xA4\xA7\xE5\xB0\x8F",
	[STR_ASPECT_RESET]      = "\xE6\x81\xA2\xE5\xA4\x8D\xE9\xBB\x98\xE8\xAE\xA4",
	[STR_HINT_ASPECT]       = "\xE5\xB7\xA6\x2F\xE5\x8F\xB3\x3A\x20\xE6\x9B\xB4\xE6\x94\xB9\x20\x20\x20\x41\x3A\x20\xE9\x80\x89\xE6\x8B\xA9\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE8\xBF\x94\xE5\x9B\x9E",
	[STR_HINT_ASPECT_EDIT]  = "\xE6\x96\xB9\xE5\x90\x91\xE9\x94\xAE\x3A\x20\xE7\xA7\xBB\xE5\x8A\xA8\x20\x20\x20\x4C\x42\x2F\x52\x42\x3A\x20\xE5\xAE\xBD\xE5\xBA\xA6\x20\x20\x20\x4C\x54\x2F\x52\x54\x3A\x20\xE9\xAB\x98\xE5\xBA\xA6\x20\x20\x20\x41\x3A\x20\xE4\xBF\x9D\xE5\xAD\x98\x20\x20\x20\x42\x3A\x20\xE5\x8F\x96\xE6\xB6\x88",
};

static const char *strings_hi[STR_COUNT] = {
	[STR_SETTINGS]        = "\xE0\xA4\xB8\xE0\xA5\x87\xE0\xA4\x9F\xE0\xA4\xBF\xE0\xA4\x82\xE0\xA4\x97\xE0\xA5\x8D\xE0\xA4\xB8",
	[STR_RESUME]          = "\xE0\xA4\x9C\xE0\xA4\xBE\xE0\xA4\xB0\xE0\xA5\x80\x20\xE0\xA4\xB0\xE0\xA4\x96\xE0\xA5\x87\xE0\xA4\x82",
	[STR_VIDEO_SETTINGS]  = "\xE0\xA4\xB5\xE0\xA5\x80\xE0\xA4\xA1\xE0\xA4\xBF\xE0\xA4\xAF\xE0\xA5\x8B",
	[STR_AUDIO_SETTINGS]  = "\xE0\xA4\x91\xE0\xA4\xA1\xE0\xA4\xBF\xE0\xA4\xAF\xE0\xA5\x8B",
	[STR_INPUT_REMAP]     = "\xE0\xA4\x95\xE0\xA4\x82\xE0\xA4\x9F\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x8B\xE0\xA4\xB2\x20\x2F\x20\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA4\xAE\xE0\xA5\x88\xE0\xA4\xAA",
	[STR_CORE_OPTIONS]    = "\xE0\xA4\x95\xE0\xA5\x8B\xE0\xA4\xB0\x20\xE0\xA4\xB5\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xB2\xE0\xA5\x8D\xE0\xA4\xAA",
	[STR_EXIT_GAME]       = "\xE0\xA4\x97\xE0\xA5\x87\xE0\xA4\xAE\x20\xE0\xA4\xB8\xE0\xA5\x87\x20\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\xB9\xE0\xA4\xB0\x20\xE0\xA4\xA8\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xB2\xE0\xA5\x87\xE0\xA4\x82",
	[STR_LANGUAGE]        = "\xE0\xA4\xAD\xE0\xA4\xBE\xE0\xA4\xB7\xE0\xA4\xBE",
	[STR_HINT_MAIN]       = "\x41\x2F\x45\x6E\x74\x65\x72\x3A\x20\xE0\xA4\x9A\xE0\xA5\x81\xE0\xA4\xA8\xE0\xA5\x87\xE0\xA4\x82\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE0\xA4\xAC\xE0\xA4\x82\xE0\xA4\xA6\x20\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x87\xE0\xA4\x82",

	[STR_VIDEO_HEADER]    = "\xE0\xA4\xB5\xE0\xA5\x80\xE0\xA4\xA1\xE0\xA4\xBF\xE0\xA4\xAF\xE0\xA5\x8B\x20\xE0\xA4\xB8\xE0\xA5\x87\xE0\xA4\x9F\xE0\xA4\xBF\xE0\xA4\x82\xE0\xA4\x97\xE0\xA5\x8D\xE0\xA4\xB8",
	[STR_FULLSCREEN]      = "\xE0\xA4\xAB\xE0\xA5\x81\xE0\xA4\xB2\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA4\xA8",
	[STR_SHADER]          = "\xE0\xA4\xB6\xE0\xA5\x87\xE0\xA4\xA1\xE0\xA4\xB0",
	[STR_FILTER]          = "\xE0\xA4\xAB\xE0\xA4\xBC\xE0\xA4\xBF\xE0\xA4\xB2\xE0\xA5\x8D\xE0\xA4\x9F\xE0\xA4\xB0",
	[STR_BACK]            = "\xE0\xA4\xB5\xE0\xA4\xBE\xE0\xA4\xAA\xE0\xA4\xB8",
	[STR_ON]              = "\xE0\xA4\x9A\xE0\xA4\xBE\xE0\xA4\xB2\xE0\xA5\x82",
	[STR_OFF]             = "\xE0\xA4\xAC\xE0\xA4\x82\xE0\xA4\xA6",
	[STR_HINT_VIDEO]      = "\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x2F\xE0\xA4\xA6\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x3A\x20\xE0\xA4\xAC\xE0\xA4\xA6\xE0\xA4\xB2\xE0\xA5\x87\xE0\xA4\x82\x20\x20\x20\x41\x2F\x45\x6E\x74\x65\x72\x3A\x20\xE0\xA4\x9F\xE0\xA5\x89\xE0\xA4\x97\xE0\xA4\xB2\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE0\xA4\xB5\xE0\xA4\xBE\xE0\xA4\xAA\xE0\xA4\xB8",

	[STR_AUDIO_HEADER]    = "\xE0\xA4\x91\xE0\xA4\xA1\xE0\xA4\xBF\xE0\xA4\xAF\xE0\xA5\x8B\x20\xE0\xA4\xB8\xE0\xA5\x87\xE0\xA4\x9F\xE0\xA4\xBF\xE0\xA4\x82\xE0\xA4\x97\xE0\xA5\x8D\xE0\xA4\xB8",
	[STR_VOLUME]          = "\xE0\xA4\x86\xE0\xA4\xB5\xE0\xA4\xBE\xE0\xA4\x9C\xE0\xA4\xBC",
	[STR_MUTE]            = "\xE0\xA4\xAE\xE0\xA5\x8D\xE0\xA4\xAF\xE0\xA5\x82\xE0\xA4\x9F",
	[STR_HINT_AUDIO]      = "\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x2F\xE0\xA4\xA6\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x3A\x20\xE0\xA4\x86\xE0\xA4\xB5\xE0\xA4\xBE\xE0\xA4\x9C\xE0\xA4\xBC\x20\xE0\xA4\xAC\xE0\xA4\xA6\xE0\xA4\xB2\xE0\xA5\x87\xE0\xA4\x82\x20\x20\x20\x41\x2F\x45\x6E\x74\x65\x72\x3A\x20\xE0\xA4\xAE\xE0\xA5\x8D\xE0\xA4\xAF\xE0\xA5\x82\xE0\xA4\x9F\x20\xE0\xA4\x9F\xE0\xA5\x89\xE0\xA4\x97\xE0\xA4\xB2\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE0\xA4\xB5\xE0\xA4\xBE\xE0\xA4\xAA\xE0\xA4\xB8",

	[STR_INPUT_HEADER]    = "\xE0\xA4\x95\xE0\xA4\x82\xE0\xA4\x9F\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x8B\xE0\xA4\xB2\x20\x2F\x20\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA4\xAE\xE0\xA5\x88\xE0\xA4\xAA",
	[STR_CONTROLLER_PORT] = "\xE0\xA4\x95\xE0\xA4\x82\xE0\xA4\x9F\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x8B\xE0\xA4\xB2\xE0\xA4\xB0\x20\xE0\xA4\xAA\xE0\xA5\x8B\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\x9F",
	[STR_PRESS_BUTTON]    = "\x5B\xE0\xA4\x95\xE0\xA5\x8B\xE0\xA4\x88\x20\xE0\xA4\xAC\xE0\xA4\x9F\xE0\xA4\xA8\x20\xE0\xA4\xA6\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x2E\x2E\x2E\x5D",
	[STR_RESET_DEFAULTS]  = "\xE0\xA4\xA1\xE0\xA4\xBF\xE0\xA4\xAB\xE0\xA4\xBC\xE0\xA5\x89\xE0\xA4\xB2\xE0\xA5\x8D\xE0\xA4\x9F\x20\xE0\xA4\xAA\xE0\xA5\x81\xE0\xA4\xA8\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA5\xE0\xA4\xBE\xE0\xA4\xAA\xE0\xA4\xBF\xE0\xA4\xA4\x20\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x87\xE0\xA4\x82",
	[STR_HINT_INPUT]      = "\x41\x2F\x45\x6E\x74\x65\x72\x3A\x20\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA4\xAE\xE0\xA5\x88\xE0\xA4\xAA\x20\x20\x20\x4C\x2F\x52\x3A\x20\xE0\xA4\xAA\xE0\xA5\x8B\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\x9F\x20\xE0\xA4\xAC\xE0\xA4\xA6\xE0\xA4\xB2\xE0\xA5\x87\xE0\xA4\x82\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE0\xA4\xB5\xE0\xA4\xBE\xE0\xA4\xAA\xE0\xA4\xB8",

	[STR_BTN_A]      = "A",      [STR_BTN_B]     = "B",
	[STR_BTN_X]      = "X",      [STR_BTN_Y]     = "Y",
	[STR_BTN_UP]     = "\xE0\xA4\x8A\xE0\xA4\xAA\xE0\xA4\xB0",
	[STR_BTN_DOWN]   = "\xE0\xA4\xA8\xE0\xA5\x80\xE0\xA4\x9A\xE0\xA5\x87",
	[STR_BTN_LEFT]   = "\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81",
	[STR_BTN_RIGHT]  = "\xE0\xA4\xA6\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81",
	[STR_BTN_START]  = "START",  [STR_BTN_SELECT]= "SELECT",
	[STR_BTN_L]      = "L",      [STR_BTN_R]     = "R",
	[STR_BTN_L2]     = "L2",     [STR_BTN_R2]    = "R2",
	[STR_BTN_L3]     = "L3",     [STR_BTN_R3]    = "R3",

	[STR_CORE_OPT_HEADER] = "\xE0\xA4\x95\xE0\xA5\x8B\xE0\xA4\xB0\x20\xE0\xA4\xB5\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xB2\xE0\xA5\x8D\xE0\xA4\xAA",
	[STR_NO_CORE_OPTIONS] = "\xE0\xA4\x95\xE0\xA5\x8B\xE0\xA4\x88\x20\xE0\xA4\x95\xE0\xA5\x8B\xE0\xA4\xB0\x20\xE0\xA4\xB5\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xB2\xE0\xA5\x8D\xE0\xA4\xAA\x20\xE0\xA4\x89\xE0\xA4\xAA\xE0\xA4\xB2\xE0\xA4\xAC\xE0\xA5\x8D\xE0\xA4\xA7\x20\xE0\xA4\xA8\xE0\xA4\xB9\xE0\xA5\x80\xE0\xA4\x82\x20\xE0\xA4\xB9\xE0\xA5\x88\xE0\xA5\xA4",
	[STR_HINT_CORE_OPTIONS] = "\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x2F\xE0\xA4\xA6\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x3A\x20\xE0\xA4\xAC\xE0\xA4\xA6\xE0\xA4\xB2\xE0\xA5\x87\xE0\xA4\x82\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE0\xA4\xB5\xE0\xA4\xBE\xE0\xA4\xAA\xE0\xA4\xB8",

	[STR_EXIT_CONFIRM]    = "\xE0\xA4\x97\xE0\xA5\x87\xE0\xA4\xAE\x20\xE0\xA4\xB8\xE0\xA5\x87\x20\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\xB9\xE0\xA4\xB0\x20\xE0\xA4\xA8\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xB2\xE0\xA5\x87\xE0\xA4\x82\x3F",
	[STR_YES_QUIT]        = "\xE0\xA4\xB9\xE0\xA4\xBE\xE0\xA4\x81\x20\x2D\x20\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\xB9\xE0\xA4\xB0\x20\xE0\xA4\xA8\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xB2\xE0\xA5\x87\xE0\xA4\x82",
	[STR_NO_CANCEL]       = "\xE0\xA4\xA8\xE0\xA4\xB9\xE0\xA5\x80\xE0\xA4\x82\x20\x2D\x20\xE0\xA4\xB0\xE0\xA4\xA6\xE0\xA5\x8D\xE0\xA4\xA6\x20\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x87\xE0\xA4\x82",

	[STR_LANG_AUTO] = "\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xB5\xE0\xA4\x9A\xE0\xA4\xBE\xE0\xA4\xB2\xE0\xA4\xBF\xE0\xA4\xA4",
	[STR_LANG_PT]   = "Portugu\xC3\xAAs",
	[STR_LANG_EN]   = "English",
	[STR_LANG_ES]   = "Espa\xC3\xB1ol",
	[STR_LANG_FR]   = "Fran\xC3\xA7""ais",
	[STR_LANG_IT]   = "Italiano",
	[STR_LANG_DE]   = "Deutsch",
	[STR_LANG_ZH]   = LANG_NAME_ZH,
	[STR_LANG_HI]   = LANG_NAME_HI,

	[STR_ASPECT_HEADER]     = "\xE0\xA4\x86\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xAA\xE0\xA5\x87\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\x9F\x20\xE0\xA4\xB0\xE0\xA5\x87\xE0\xA4\xB6\xE0\xA4\xBF\xE0\xA4\xAF\xE0\xA5\x8B",
	[STR_ASPECT_RATIO]      = "\xE0\xA4\x85\xE0\xA4\xA8\xE0\xA5\x81\xE0\xA4\xAA\xE0\xA4\xBE\xE0\xA4\xA4",
	[STR_ASPECT_CORE]       = "\xE0\xA4\x95\xE0\xA5\x8B\xE0\xA4\xB0\x20\xE0\xA4\xA1\xE0\xA4\xBF\xE0\xA4\xAB\xE0\xA4\xBC\xE0\xA5\x89\xE0\xA4\xB2\xE0\xA5\x8D\xE0\xA4\x9F",
	[STR_ASPECT_STRETCH]    = "\xE0\xA4\x96\xE0\xA5\x80\xE0\xA4\x82\xE0\xA4\x9A\xE0\xA5\x87\xE0\xA4\x82",
	[STR_ASPECT_4_3]        = "4:3",
	[STR_ASPECT_16_9]       = "16:9",
	[STR_ASPECT_CUSTOM]     = "\xE0\xA4\x95\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\x9F\xE0\xA4\xAE",
	[STR_ASPECT_CUSTOM_EDIT]= "\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA5\xE0\xA4\xBF\xE0\xA4\xA4\xE0\xA4\xBF\x2F\xE0\xA4\x86\xE0\xA4\x95\xE0\xA4\xBE\xE0\xA4\xB0\x20\xE0\xA4\xB8\xE0\xA4\xAE\xE0\xA4\xBE\xE0\xA4\xAF\xE0\xA5\x8B\xE0\xA4\x9C\xE0\xA4\xBF\xE0\xA4\xA4\x20\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x87\xE0\xA4\x82",
	[STR_ASPECT_RESET]      = "\xE0\xA4\xA1\xE0\xA4\xBF\xE0\xA4\xAB\xE0\xA4\xBC\xE0\xA5\x89\xE0\xA4\xB2\xE0\xA5\x8D\xE0\xA4\x9F\x20\xE0\xA4\xAA\xE0\xA4\xB0\x20\xE0\xA4\xB0\xE0\xA5\x80\xE0\xA4\xB8\xE0\xA5\x87\xE0\xA4\x9F\x20\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x87\xE0\xA4\x82",
	[STR_HINT_ASPECT]       = "\xE0\xA4\xAC\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x2F\xE0\xA4\xA6\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x3A\x20\xE0\xA4\xAC\xE0\xA4\xA6\xE0\xA4\xB2\xE0\xA5\x87\xE0\xA4\x82\x20\x20\x20\x41\x3A\x20\xE0\xA4\x9A\xE0\xA5\x81\xE0\xA4\xA8\xE0\xA5\x87\xE0\xA4\x82\x20\x20\x20\x42\x2F\x45\x73\x63\x3A\x20\xE0\xA4\xB5\xE0\xA4\xBE\xE0\xA4\xAA\xE0\xA4\xB8",
	[STR_HINT_ASPECT_EDIT]  = "\x44\x2D\x50\x61\x64\x3A\x20\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xB2\xE0\xA4\xBE\xE0\xA4\x8F\xE0\xA4\x81\x20\x20\x20\x4C\x42\x2F\x52\x42\x3A\x20\xE0\xA4\x9A\xE0\xA5\x8C\xE0\xA4\xA1\xE0\xA4\xBC\xE0\xA4\xBE\xE0\xA4\x88\x20\x20\x20\x4C\x54\x2F\x52\x54\x3A\x20\xE0\xA4\x8A\xE0\xA4\x81\xE0\xA4\x9A\xE0\xA4\xBE\xE0\xA4\x88\x20\x20\x20\x41\x3A\x20\xE0\xA4\xB8\xE0\xA4\xB9\xE0\xA5\x87\xE0\xA4\x9C\xE0\xA5\x87\xE0\xA4\x82\x20\x20\x20\x42\x3A\x20\xE0\xA4\xB0\xE0\xA4\xA6\xE0\xA5\x8D\xE0\xA4\xA6\x20\xE0\xA4\x95\xE0\xA4\xB0\xE0\xA5\x87\xE0\xA4\x82",
};

/* Tabela de ponteiros para tabelas de strings */
static const char **all_strings[LANG_COUNT] = {
	strings_pt, strings_en, strings_es, strings_fr, strings_it, strings_de,
	strings_zh, strings_hi
};

/* ─────────────────── Detecção Automática ─────────────────── */

static language_t detect_system_language(void)
{
#ifdef _WIN32
	LANGID lid = GetUserDefaultUILanguage();
	unsigned primary = lid & 0xFF;

	switch (primary) {
		case 0x16: return LANG_PT;  /* Portuguese */
		case 0x09: return LANG_EN;  /* English */
		case 0x0A: return LANG_ES;  /* Spanish */
		case 0x0C: return LANG_FR;  /* French */
		case 0x10: return LANG_IT;  /* Italian */
		case 0x07: return LANG_DE;  /* German */
		case 0x04: return LANG_ZH;  /* Chinese */
		case 0x39: return LANG_HI;  /* Hindi */
		default:   return LANG_PT;  /* Fallback para PT-BR */
	}
#else
	/* Unix: verificar variáveis de ambiente */
	const char *lc = getenv("LANG");
	if (!lc) lc = getenv("LC_ALL");
	if (!lc) return LANG_PT;

	if (strncmp(lc, "pt", 2) == 0) return LANG_PT;
	if (strncmp(lc, "en", 2) == 0) return LANG_EN;
	if (strncmp(lc, "es", 2) == 0) return LANG_ES;
	if (strncmp(lc, "fr", 2) == 0) return LANG_FR;
	if (strncmp(lc, "it", 2) == 0) return LANG_IT;
	if (strncmp(lc, "de", 2) == 0) return LANG_DE;
	if (strncmp(lc, "zh", 2) == 0) return LANG_ZH;
	if (strncmp(lc, "hi", 2) == 0) return LANG_HI;

	return LANG_PT;
#endif
}

static int lang_code_matches(const char *lang_setting, const char *code)
{
	size_t i = 0;

	if (!lang_setting || !code)
		return 0;

	while (code[i] && lang_setting[i]) {
		unsigned char a = (unsigned char)lang_setting[i];
		unsigned char b = (unsigned char)code[i];

		if (tolower(a) != tolower(b))
			return 0;
		i++;
	}

	if (code[i] != '\0')
		return 0;

	return lang_setting[i] == '\0' || lang_setting[i] == '_' || lang_setting[i] == '-';
}

/* ─────────────────── API Pública ─────────────────── */

void lang_init(const char *lang_setting)
{
	if (!lang_setting || strcmp(lang_setting, "auto") == 0) {
		current_lang = detect_system_language();
	} else if (lang_code_matches(lang_setting, "pt")) {
		current_lang = LANG_PT;
	} else if (lang_code_matches(lang_setting, "en")) {
		current_lang = LANG_EN;
	} else if (lang_code_matches(lang_setting, "es")) {
		current_lang = LANG_ES;
	} else if (lang_code_matches(lang_setting, "fr")) {
		current_lang = LANG_FR;
	} else if (lang_code_matches(lang_setting, "it")) {
		current_lang = LANG_IT;
	} else if (lang_code_matches(lang_setting, "de")) {
		current_lang = LANG_DE;
	} else if (lang_code_matches(lang_setting, "zh")) {
		current_lang = LANG_ZH;
	} else if (lang_code_matches(lang_setting, "hi")) {
		current_lang = LANG_HI;
	} else {
		current_lang = detect_system_language();
	}
	printf("Idioma: %s (%s)\n", lang_display_name(current_lang), lang_code(current_lang));
}

const char *lang_get(string_id id)
{
	if (id < 0 || id >= STR_COUNT) return "???";
	const char *s = all_strings[current_lang][id];
	if (!s) s = all_strings[LANG_EN][id]; /* Fallback para inglês */
	if (!s) s = "???";
	return s;
}

void lang_set(language_t lang)
{
	if (lang >= 0 && lang < LANG_COUNT)
		current_lang = lang;
}

language_t lang_current(void)
{
	return current_lang;
}

const char *lang_code(language_t lang)
{
	switch (lang) {
		case LANG_PT: return "pt";
		case LANG_EN: return "en";
		case LANG_ES: return "es";
		case LANG_FR: return "fr";
		case LANG_IT: return "it";
		case LANG_DE: return "de";
		case LANG_ZH: return "zh";
		case LANG_HI: return "hi";
		default: return "pt";
	}
}

const char *lang_display_name(language_t lang)
{
	switch (lang) {
		case LANG_PT: return "Portugu\xC3\xAAs";
		case LANG_EN: return "English";
		case LANG_ES: return "Espa\xC3\xB1ol";
		case LANG_FR: return "Fran\xC3\xA7""ais";
		case LANG_IT: return "Italiano";
		case LANG_DE: return "Deutsch";
		case LANG_ZH: return LANG_NAME_ZH;
		case LANG_HI: return LANG_NAME_HI;
		default: return "Portugu\xC3\xAAs";
	}
}

void lang_cycle(int dir)
{
	int l = (int)current_lang + dir;
	if (l < 0) l = LANG_COUNT - 1;
	if (l >= LANG_COUNT) l = 0;
	current_lang = (language_t)l;
}

unsigned lang_to_retro(void)
{
	switch (current_lang) {
		case LANG_PT: return RETRO_LANGUAGE_PORTUGUESE_BRAZIL;
		case LANG_EN: return RETRO_LANGUAGE_ENGLISH;
		case LANG_ES: return RETRO_LANGUAGE_SPANISH;
		case LANG_FR: return RETRO_LANGUAGE_FRENCH;
		case LANG_IT: return RETRO_LANGUAGE_ITALIAN;
		case LANG_DE: return RETRO_LANGUAGE_GERMAN;
		case LANG_ZH: return RETRO_LANGUAGE_CHINESE_SIMPLIFIED;
		case LANG_HI: return RETRO_LANGUAGE_ENGLISH;
		default: return RETRO_LANGUAGE_ENGLISH;
	}
}
