#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include "lang.h"

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#endif

#include "libretro.h"
#include "utils.h"
#include "video.h"
#include "audio.h"
#include "input.h"
#include "options.h"
#include "config.h"

#if defined(_WIN32)
#define load_lib(L) LoadLibrary(L);
#define load_sym(V, S) ((*(void**)&V) = GetProcAddress(core.handle, #S))
#define close_lib(L) //(L)
#else
#define load_sym(V, S) do {\
	if (!((*(void**)&V) = dlsym(core.handle, #S))) \
		die("Failed to load symbol '" #S "'': %s", dlerror()); \
	} while (0)
#define load_lib(L) dlopen(L, RTLD_LAZY);
#define close_lib(L) dlclose(L);
#endif
#define load_retro_sym(S) load_sym(core.S, S)

static struct {
	void *handle;
	bool initialized;

	void (*retro_init)(void);
	void (*retro_deinit)(void);
	unsigned (*retro_api_version)(void);
	void (*retro_get_system_info)(struct retro_system_info *info);
	void (*retro_get_system_av_info)(struct retro_system_av_info *info);
	void (*retro_set_controller_port_device)(unsigned port, unsigned device);
	void (*retro_reset)(void);
	void (*retro_run)(void);
	bool (*retro_load_game)(const struct retro_game_info *game);
	void (*retro_unload_game)(void);
	void* (*retro_get_memory_data)(unsigned);
	size_t (*retro_get_memory_size)(unsigned);
} core;

static struct retro_frame_time_callback runloop_frame_time;
static retro_usec_t runloop_frame_time_last = 0;
static unsigned long long core_run_count = 0;
static float core_nominal_fps = 60.0f;
static bool ppsspp_save_root_prepared = false;
static const struct retro_input_descriptor *input_descriptors = NULL;
static const struct retro_controller_info *controller_infos = NULL;
static const struct retro_subsystem_info *subsystem_infos = NULL;
static struct retro_memory_map memory_map = {0};
static struct retro_core_options_update_display_callback options_display_cb = {0};
extern config g_cfg;

#define VFS_PATH_MAX 4096

struct retro_vfs_file_handle {
	FILE *fp;
	char path[VFS_PATH_MAX];
};

struct retro_vfs_dir_handle {
	bool include_hidden;
	char dirpath[VFS_PATH_MAX];
	char name[VFS_PATH_MAX];
	bool is_dir;
#if defined(_WIN32)
	HANDLE handle;
	WIN32_FIND_DATAA data;
	bool first;
#else
	DIR *dir;
#endif
};

#if defined(_WIN32)
#define vfs_fseek _fseeki64
#define vfs_ftell _ftelli64
#else
#define vfs_fseek fseeko
#define vfs_ftell ftello
#endif

static unsigned count_input_descriptors(const struct retro_input_descriptor *desc)
{
	unsigned count = 0;

	if (!desc)
		return 0;

	while (desc[count].description)
		count++;

	return count;
}

static unsigned count_controller_infos(const struct retro_controller_info *info)
{
	unsigned count = 0;

	if (!info)
		return 0;

	while (info[count].types || info[count].num_types)
		count++;

	return count;
}

static unsigned count_subsystem_infos(const struct retro_subsystem_info *info)
{
	unsigned count = 0;

	if (!info)
		return 0;

	while (info[count].desc || info[count].ident || info[count].roms ||
		info[count].num_roms || info[count].id)
		count++;

	return count;
}

static bool should_log_variable_lookup(const char *key)
{
	if (!key)
		return false;

	return strcmp(key, "mupen64plus-rdp-plugin") == 0 ||
		strcmp(key, "mupen64plus-rsp-plugin") == 0 ||
		strcmp(key, "mupen64plus-ThreadedRenderer") == 0 ||
		strcmp(key, "mupen64plus-EnableFBEmulation") == 0;
}

static bool core_path_contains(const char *needle);

static bool core_prefers_vulkan_hw(void)
{
	if (core_path_contains("mupen64plus_next") || core_path_contains("mupen64plus"))
		return false;

	return g_cfg.core != NULL;
}

static bool core_path_contains(const char *needle)
{
	return g_cfg.core && needle && strstr(g_cfg.core, needle) != NULL;
}

static bool ensure_directory_exists(const char *path)
{
	int rc = 0;

	if (!path || !*path)
		return false;

#if defined(_WIN32)
	rc = _mkdir(path);
#else
	rc = mkdir(path, 0755);
#endif

	if (rc == 0 || errno == EEXIST)
		return true;

	log_printf("core", "failed to create directory '%s': %s", path, strerror(errno));
	return false;
}

static void vfs_normalize_path(char *dst, size_t dst_size, const char *src)
{
	size_t i = 0;

	if (!dst || !dst_size) {
		return;
	}

	dst[0] = '\0';

	if (!src) {
		return;
	}

	snprintf(dst, dst_size, "%s", src);

#if defined(_WIN32)
	for (i = 0; dst[i]; i++) {
		if (dst[i] == '/')
			dst[i] = '\\';
	}
#else
	(void)i;
#endif
}

static void vfs_resolve_path(char *dst, size_t dst_size, const char *src)
{
	char normalized[VFS_PATH_MAX];

	if (!dst || !dst_size) {
		return;
	}

	dst[0] = '\0';
	vfs_normalize_path(normalized, sizeof(normalized), src);

	if (!normalized[0]) {
		return;
	}

#if defined(_WIN32)
	{
		DWORD rc = GetFullPathNameA(normalized, (DWORD)dst_size, dst, NULL);
		if (rc > 0 && rc < dst_size)
			return;
	}
#else
	if (normalized[0] == '/')
	{
		snprintf(dst, dst_size, "%s", normalized);
		return;
	}
	else
	{
		char cwd[VFS_PATH_MAX];
		if (getcwd(cwd, sizeof(cwd))) {
			snprintf(dst, dst_size, "%s/%s", cwd, normalized);
			return;
		}
	}
#endif

	snprintf(dst, dst_size, "%s", normalized);
}

static bool vfs_is_write_mode(unsigned mode)
{
	return (mode & RETRO_VFS_FILE_ACCESS_WRITE) != 0;
}

static int vfs_mkdir_single(const char *path)
{
	int rc = 0;

	if (!path || !*path)
		return -1;

#if defined(_WIN32)
	rc = _mkdir(path);
#else
	rc = mkdir(path, 0755);
#endif

	if (rc == 0)
		return 0;

	if (errno == EEXIST)
		return -2;

	return -1;
}

static bool vfs_extract_parent_path(const char *path, char *parent, size_t parent_size)
{
	char *slash = NULL;

	if (!parent || parent_size == 0) {
		return false;
	}

	parent[0] = '\0';

	if (!path || !*path) {
		return false;
	}

	snprintf(parent, parent_size, "%s", path);
	slash = strrchr(parent, '/');
#if defined(_WIN32)
	{
		char *backslash = strrchr(parent, '\\');
		if (!slash || (backslash && backslash > slash))
			slash = backslash;
	}
#endif

	if (!slash)
		return false;

	*slash = '\0';
	return parent[0] != '\0';
}

static int vfs_mkdir_recursive(const char *dir)
{
	char path[VFS_PATH_MAX];
	size_t i = 0;
	int rc = 0;

	vfs_normalize_path(path, sizeof(path), dir);

	if (!path[0])
		return -1;

	for (i = 0; path[i]; i++) {
		if (path[i] != '/' && path[i] != '\\')
			continue;

		if (i == 0)
			continue;

		if (path[i - 1] == ':' || path[i - 1] == '/' || path[i - 1] == '\\')
			continue;

		{
			char saved = path[i];
			path[i] = '\0';
			rc = vfs_mkdir_single(path);
			path[i] = saved;

			if (rc == -1)
				return -1;
		}
	}

	return vfs_mkdir_single(path);
}

static const char *retro_vfs_file_get_path_impl(struct retro_vfs_file_handle *stream)
{
	if (!stream)
		return NULL;
	return stream->path;
}

static struct retro_vfs_file_handle *retro_vfs_file_open_impl(const char *path, unsigned mode, unsigned hints)
{
	struct retro_vfs_file_handle *stream = NULL;
	char native_path[VFS_PATH_MAX];
	const char *fmode = "rb";
	(void)hints;

	if (!path)
		return NULL;

	if ((mode & RETRO_VFS_FILE_ACCESS_READ_WRITE) == RETRO_VFS_FILE_ACCESS_READ_WRITE) {
		fmode = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? "r+b" : "w+b";
	} else if (mode & RETRO_VFS_FILE_ACCESS_WRITE) {
		fmode = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) ? "r+b" : "wb";
	} else if (mode & RETRO_VFS_FILE_ACCESS_READ) {
		fmode = "rb";
	}

	vfs_resolve_path(native_path, sizeof(native_path), path);

	stream = (struct retro_vfs_file_handle *)calloc(1, sizeof(*stream));
	if (!stream)
		return NULL;

	snprintf(stream->path, sizeof(stream->path), "%s", native_path);
	stream->fp = fopen(native_path, fmode);

	if (!stream->fp && vfs_is_write_mode(mode)) {
		char parent[VFS_PATH_MAX];
		if (vfs_extract_parent_path(native_path, parent, sizeof(parent)))
			vfs_mkdir_recursive(parent);
		stream->fp = fopen(native_path, fmode);
	}

	if (!stream->fp) {
		free(stream);
		return NULL;
	}

	return stream;
}

static int retro_vfs_file_close_impl(struct retro_vfs_file_handle *stream)
{
	int rc = -1;

	if (!stream)
		return -1;

	if (stream->fp)
		rc = fclose(stream->fp);

	free(stream);
	return rc == 0 ? 0 : -1;
}

static int64_t retro_vfs_file_size_impl(struct retro_vfs_file_handle *stream)
{
	int64_t current = 0;
	int64_t size = 0;

	if (!stream || !stream->fp)
		return -1;

	current = vfs_ftell(stream->fp);
	if (current < 0)
		return -1;

	if (vfs_fseek(stream->fp, 0, SEEK_END) != 0)
		return -1;

	size = vfs_ftell(stream->fp);
	if (size < 0)
		return -1;

	if (vfs_fseek(stream->fp, current, SEEK_SET) != 0)
		return -1;

	return size;
}

static int64_t retro_vfs_file_truncate_impl(struct retro_vfs_file_handle *stream, int64_t length)
{
	if (!stream || !stream->fp)
		return -1;

#if defined(_WIN32)
	return _chsize_s(_fileno(stream->fp), length) == 0 ? 0 : -1;
#else
	return ftruncate(fileno(stream->fp), length) == 0 ? 0 : -1;
#endif
}

static int64_t retro_vfs_file_tell_impl(struct retro_vfs_file_handle *stream)
{
	if (!stream || !stream->fp)
		return -1;
	return vfs_ftell(stream->fp);
}

static int64_t retro_vfs_file_seek_impl(struct retro_vfs_file_handle *stream, int64_t offset, int seek_position)
{
	if (!stream || !stream->fp)
		return -1;

	if (vfs_fseek(stream->fp, offset, seek_position) != 0)
		return -1;

	return vfs_ftell(stream->fp);
}

static int64_t retro_vfs_file_read_impl(struct retro_vfs_file_handle *stream, void *s, uint64_t len)
{
	size_t bytes = 0;

	if (!stream || !stream->fp || (!s && len))
		return -1;

	bytes = fread(s, 1, (size_t)len, stream->fp);
	if (bytes == 0 && ferror(stream->fp))
		return -1;

	return (int64_t)bytes;
}

static int64_t retro_vfs_file_write_impl(struct retro_vfs_file_handle *stream, const void *s, uint64_t len)
{
	size_t bytes = 0;

	if (!stream || !stream->fp || (!s && len))
		return -1;

	bytes = fwrite(s, 1, (size_t)len, stream->fp);
	if (bytes == 0 && ferror(stream->fp))
		return -1;

	return (int64_t)bytes;
}

static int retro_vfs_file_flush_impl(struct retro_vfs_file_handle *stream)
{
	if (!stream || !stream->fp)
		return -1;
	return fflush(stream->fp) == 0 ? 0 : -1;
}

static int retro_vfs_file_remove_impl(const char *path)
{
	char native_path[VFS_PATH_MAX];

	if (!path)
		return -1;

	vfs_resolve_path(native_path, sizeof(native_path), path);
	return remove(native_path) == 0 ? 0 : -1;
}

static int retro_vfs_file_rename_impl(const char *old_path, const char *new_path)
{
	char old_native[VFS_PATH_MAX];
	char new_native[VFS_PATH_MAX];

	if (!old_path || !new_path)
		return -1;

	vfs_resolve_path(old_native, sizeof(old_native), old_path);
	vfs_resolve_path(new_native, sizeof(new_native), new_path);
	return rename(old_native, new_native) == 0 ? 0 : -1;
}

static int retro_vfs_stat_impl(const char *path, int32_t *size)
{
	char native_path[VFS_PATH_MAX];

	if (!path)
		return 0;

	vfs_resolve_path(native_path, sizeof(native_path), path);

#if defined(_WIN32)
	{
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!GetFileAttributesExA(native_path, GetFileExInfoStandard, &data))
			return 0;

		if (size) {
			LARGE_INTEGER file_size;
			file_size.HighPart = (LONG)data.nFileSizeHigh;
			file_size.LowPart = data.nFileSizeLow;
			*size = (int32_t)file_size.QuadPart;
		}

		return RETRO_VFS_STAT_IS_VALID |
			((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? RETRO_VFS_STAT_IS_DIRECTORY : 0);
	}
#else
	{
		struct stat st;
		if (stat(native_path, &st) != 0)
			return 0;

		if (size)
			*size = (int32_t)st.st_size;

		return RETRO_VFS_STAT_IS_VALID | (S_ISDIR(st.st_mode) ? RETRO_VFS_STAT_IS_DIRECTORY : 0);
	}
#endif
}

static int retro_vfs_mkdir_impl(const char *dir)
{
	return vfs_mkdir_recursive(dir);
}

static struct retro_vfs_dir_handle *retro_vfs_opendir_impl(const char *dir, bool include_hidden)
{
	struct retro_vfs_dir_handle *dirstream = NULL;

	if (!dir)
		return NULL;

	dirstream = (struct retro_vfs_dir_handle *)calloc(1, sizeof(*dirstream));
	if (!dirstream)
		return NULL;

	dirstream->include_hidden = include_hidden;
	vfs_resolve_path(dirstream->dirpath, sizeof(dirstream->dirpath), dir);

#if defined(_WIN32)
	{
		char native_path[VFS_PATH_MAX];
		char pattern[VFS_PATH_MAX];
		size_t len = 0;

		snprintf(native_path, sizeof(native_path), "%s", dirstream->dirpath);
		snprintf(pattern, sizeof(pattern), "%s", native_path);
		len = strlen(pattern);
		if (len > 0 && pattern[len - 1] != '\\' && pattern[len - 1] != '/')
			snprintf(pattern + len, sizeof(pattern) - len, "\\*");
		else
			snprintf(pattern + len, sizeof(pattern) - len, "*");

		dirstream->handle = FindFirstFileA(pattern, &dirstream->data);
		if (dirstream->handle == INVALID_HANDLE_VALUE) {
			free(dirstream);
			return NULL;
		}
		dirstream->first = true;
	}
#else
	{
		char native_path[VFS_PATH_MAX];
		snprintf(native_path, sizeof(native_path), "%s", dirstream->dirpath);
		dirstream->dir = opendir(native_path);
		if (!dirstream->dir) {
			free(dirstream);
			return NULL;
		}
	}
#endif

	return dirstream;
}

static bool retro_vfs_readdir_impl(struct retro_vfs_dir_handle *dirstream)
{
	if (!dirstream)
		return false;

#if defined(_WIN32)
	for (;;) {
		WIN32_FIND_DATAA *data = &dirstream->data;
		if (dirstream->first)
			dirstream->first = false;
		else if (!FindNextFileA(dirstream->handle, data))
			return false;

		if (!dirstream->include_hidden &&
			((data->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) || data->cFileName[0] == '.'))
			continue;

		snprintf(dirstream->name, sizeof(dirstream->name), "%s", data->cFileName);
		dirstream->is_dir = (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		return true;
	}
#else
	for (;;) {
		struct dirent *entry = readdir(dirstream->dir);
		if (!entry)
			return false;

		if (!dirstream->include_hidden && entry->d_name[0] == '.')
			continue;

		snprintf(dirstream->name, sizeof(dirstream->name), "%s", entry->d_name);

#ifdef DT_DIR
		if (entry->d_type != DT_UNKNOWN) {
			dirstream->is_dir = entry->d_type == DT_DIR;
		} else
#endif
		{
			char full_path[VFS_PATH_MAX];
			struct stat st;
			snprintf(full_path, sizeof(full_path), "%s/%s", dirstream->dirpath, entry->d_name);
			dirstream->is_dir = stat(full_path, &st) == 0 && S_ISDIR(st.st_mode);
		}

		return true;
	}
#endif
}

static const char *retro_vfs_dirent_get_name_impl(struct retro_vfs_dir_handle *dirstream)
{
	if (!dirstream)
		return NULL;
	return dirstream->name;
}

static bool retro_vfs_dirent_is_dir_impl(struct retro_vfs_dir_handle *dirstream)
{
	if (!dirstream)
		return false;
	return dirstream->is_dir;
}

static int retro_vfs_closedir_impl(struct retro_vfs_dir_handle *dirstream)
{
	if (!dirstream)
		return -1;

#if defined(_WIN32)
	if (dirstream->handle != INVALID_HANDLE_VALUE)
		FindClose(dirstream->handle);
#else
	if (dirstream->dir)
		closedir(dirstream->dir);
#endif

	free(dirstream);
	return 0;
}

static bool prepare_ppsspp_save_root(void)
{
	static const char *paths[] = {
		"./save",
		"./save/PSP",
		"./save/PSP/SAVEDATA",
		"./save/PSP/SYSTEM",
		"./save/PSP/SYSTEM/CACHE",
		"./save/PSP/Cheats",
		"./save/PSP/GAME",
		"./save/PSP/TEXTURES",
		"./save/PSP/PPSSPP_STATE",
		"./save/PSP/flash0",
	};
	bool ok = true;
	size_t i = 0;

	for (i = 0; i < (sizeof(paths) / sizeof(paths[0])); i++) {
		if (!ensure_directory_exists(paths[i]))
			ok = false;
	}

	return ok;
}

static const char *core_system_directory(void)
{
	return ".";
}

static const char *core_save_directory(void)
{
	if (core_path_contains("ppsspp")) {
		if (!ppsspp_save_root_prepared) {
			bool created = prepare_ppsspp_save_root();
			log_printf("core", "PPSSPP save root '%s' prepared=%s (RetroArch layout)",
				"./save",
				created ? "yes" : "no");
			ppsspp_save_root_prepared = true;
		}
		return "./save";
	}

	return ".";
}

static const char *core_assets_directory(void)
{
	return core_system_directory();
}

static bool core_set_rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
	(void)port;
	(void)effect;
	(void)strength;
	return false;
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	char buffer[4096] = {0};
	static const char * levelstr[] = { "dbg", "inf", "wrn", "err" };
	va_list va;

	va_start(va, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, va);
	va_end(va);

	if ((unsigned)level >= (sizeof(levelstr) / sizeof(levelstr[0])))
		level = RETRO_LOG_DEBUG;

	fprintf(stderr, "[%s] %s", levelstr[level], buffer);
	fflush(stderr);
}

static retro_time_t get_time_usec()
{
	struct timeval tv;
	gettimeofday(&tv,NULL);
	return tv.tv_sec*(int64_t)1000000+tv.tv_usec;
}

static bool core_environment(unsigned cmd, void *data)
{
	switch (cmd) {
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
			struct retro_log_callback *cb = (struct retro_log_callback *)data;
			cb->log = core_log;
		}
		break;
		case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
			*(bool*)data = true;
		}
		break;

		case RETRO_ENVIRONMENT_GET_USERNAME: {
			*(const char**)data = NULL;
		}
		break;
		case RETRO_ENVIRONMENT_GET_FASTFORWARDING: {
			if (!data)
				return false;
			*(bool*)data = input_is_fast_forward();
			return true;
		}
		break;
		case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
			struct retro_throttle_state *state = (struct retro_throttle_state *)data;
			float rate = core_nominal_fps;

			if (!state)
				return false;

			if (rate <= 0.0f)
				rate = 60.0f;

			if (input_is_fast_forward()) {
				int ff_ratio = g_cfg.ff_speed > 1 ? g_cfg.ff_speed : 1;
				state->mode = RETRO_THROTTLE_FAST_FORWARD;
				state->rate = rate * (float)ff_ratio;
			} else if ((!video_uses_vulkan()) && g_cfg.swap_interval > 0) {
				state->mode = RETRO_THROTTLE_VSYNC;
				state->rate = rate;
			} else {
				state->mode = RETRO_THROTTLE_NONE;
				state->rate = rate;
			}

			return true;
		}
		case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: {
			struct retro_rumble_interface *iface = (struct retro_rumble_interface *)data;

			if (!iface)
				return false;

			iface->set_rumble_state = core_set_rumble_state;
			log_printf("core", "GET_RUMBLE_INTERFACE accepted (stub)");
			return true;
		}
		case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
			input_descriptors = (const struct retro_input_descriptor *)data;
			log_printf("core", "SET_INPUT_DESCRIPTORS accepted count=%u",
				count_input_descriptors(input_descriptors));
			return true;
		}
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
			*(bool*)data = opt_updated();
			opt_clear_updated();
		}
		break;
		case RETRO_ENVIRONMENT_SHUTDOWN: {
			video_should_close(1);
		}
		break;
		case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
			*(int*)data = 1 << 0 | 1 << 1;
		}
		break;
		case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
			const struct retro_frame_time_callback *cb =
				(const struct retro_frame_time_callback*)data;
			runloop_frame_time = *cb;
		}
		break;
		case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
			const struct retro_keyboard_callback *cb =
				(const struct retro_keyboard_callback*)data;
			input_set_keyboard_callback(cb->callback);
		}
		break;
		case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
			struct retro_perf_callback *perf_cb = (struct retro_perf_callback*)data;
			perf_cb->get_time_usec = get_time_usec;
		}
		break;
		case RETRO_ENVIRONMENT_SET_GEOMETRY: {
			const struct retro_game_geometry *geom = (const struct retro_game_geometry*)data;
			video_set_geometry(geom);
		}
		break;
		case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
			const struct retro_system_av_info *av = (const struct retro_system_av_info *)data;
			return video_set_system_av_info(av);
		}
		break;
		case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO: {
			subsystem_infos = (const struct retro_subsystem_info *)data;
			log_printf("core", "SET_SUBSYSTEM_INFO accepted count=%u",
				count_subsystem_infos(subsystem_infos));
			return true;
		}
		case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {
			controller_infos = (const struct retro_controller_info *)data;
			log_printf("core", "SET_CONTROLLER_INFO accepted count=%u",
				count_controller_infos(controller_infos));
			return true;
		}
		case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
			const struct retro_memory_map *map = (const struct retro_memory_map *)data;

			if (!map)
				return false;

			memory_map = *map;
			log_printf("core", "SET_MEMORY_MAPS accepted descriptors=%u",
				memory_map.num_descriptors);
			return true;
		}
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: {
			*(unsigned *)data = 2;
		}
		break;
		case RETRO_ENVIRONMENT_GET_LANGUAGE: {
			*(unsigned *)data = lang_to_retro();
		}
		break;
		case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {
			unsigned preferred = RETRO_HW_CONTEXT_OPENGL_CORE;

			if (core_prefers_vulkan_hw() && video_hw_context_supported(RETRO_HW_CONTEXT_VULKAN))
				preferred = RETRO_HW_CONTEXT_VULKAN;

			*(unsigned *)data = preferred;
			log_printf("core", "GET_PREFERRED_HW_RENDER -> %s",
				preferred == RETRO_HW_CONTEXT_VULKAN ? "VULKAN" : "OPENGL_CORE");
		}
		break;
		case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE: {
			const struct retro_hw_render_interface **iface =
				(const struct retro_hw_render_interface **)data;

			if (!iface)
				return false;

			*iface = video_get_hw_render_interface();
			return *iface != NULL;
		}
		case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
			return video_set_hw_render_context_negotiation_interface(
				(const struct retro_hw_render_context_negotiation_interface *)data);
		case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
			return video_get_hw_render_context_negotiation_interface_support(
				(struct retro_hw_render_context_negotiation_interface *)data);
		case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
			/* Single-thread, single-context — sharing is implicit */
			log_printf("core", "SET_HW_SHARED_CONTEXT accepted");
			return true;
		case RETRO_ENVIRONMENT_SET_HW_RENDER: {
			struct retro_hw_render_callback *hw = (struct retro_hw_render_callback*)data;

			if (!hw)
				return false;

			if (!video_hw_context_supported(hw->context_type)) {
				log_printf("core",
					"SET_HW_RENDER rejected unsupported context type=%u version=%u.%u",
					hw->context_type,
					hw->version_major,
					hw->version_minor);
				return false;
			}

			hw->get_current_framebuffer = video_get_current_framebuffer;
			hw->get_proc_address = video_get_proc_address;
			video_set_hw(*hw);
			log_printf("core",
				"SET_HW_RENDER type=%u version=%u.%u depth=%d stencil=%d bottom_left=%d cache=%d debug=%d",
				hw->context_type,
				hw->version_major,
				hw->version_minor,
				hw->depth,
				hw->stencil,
				hw->bottom_left_origin,
				hw->cache_context,
				hw->debug_context);
		}
		break;
		case RETRO_ENVIRONMENT_SET_VARIABLES: {
			opt_parse_variables(data);
			return true;
		}
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
			opt_parse_v1(data);
			return true;
		}
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
			opt_parse_v2(data);
			return true;
		}
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
			opt_parse_v1_intl(data);
			return true;
		}
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
			opt_parse_v2_intl(data);
			return true;
		}
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
			const struct retro_core_option_display *display =
				(const struct retro_core_option_display *)data;
			if (!display || !display->key)
				return false;
			return opt_set_visible(display->key, display->visible);
		}
		case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {
			const struct retro_core_options_update_display_callback *cb =
				(const struct retro_core_options_update_display_callback *)data;

			if (!cb)
				return false;

			options_display_cb = *cb;
			log_printf("core", "SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK accepted registered=%s",
				options_display_cb.callback ? "yes" : "no");
			return true;
		}
		case RETRO_ENVIRONMENT_GET_VARIABLE: {
			struct retro_variable *var = (struct retro_variable*) data;
			bool found = get_option(var->key, &var->value);

			if (should_log_variable_lookup(var->key)) {
				log_printf("core", "GET_VARIABLE key='%s' found=%s value='%s'",
					var->key ? var->key : "(null)",
					found ? "yes" : "no",
					(found && var->value) ? var->value : "(null)");
			}

			return found;
		}
		break;
		case RETRO_ENVIRONMENT_SET_VARIABLE: {
			if (!data)
				return true;

			const struct retro_variable *var = (const struct retro_variable *)data;

			if (!var->key || !var->value)
				return false;

			return opt_override(var->key, var->value);
		}
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
			const enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;
			log_printf("core", "SET_PIXEL_FORMAT %u", *fmt);
			return video_set_pixel_format(*fmt);
		}
		break;
		case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
			static struct retro_vfs_interface vfs_iface = {
				retro_vfs_file_get_path_impl,
				retro_vfs_file_open_impl,
				retro_vfs_file_close_impl,
				retro_vfs_file_size_impl,
				retro_vfs_file_tell_impl,
				retro_vfs_file_seek_impl,
				retro_vfs_file_read_impl,
				retro_vfs_file_write_impl,
				retro_vfs_file_flush_impl,
				retro_vfs_file_remove_impl,
				retro_vfs_file_rename_impl,
				retro_vfs_file_truncate_impl,
				retro_vfs_stat_impl,
				retro_vfs_mkdir_impl,
				retro_vfs_opendir_impl,
				retro_vfs_readdir_impl,
				retro_vfs_dirent_get_name_impl,
				retro_vfs_dirent_is_dir_impl,
				retro_vfs_closedir_impl
			};
			struct retro_vfs_interface_info *info = (struct retro_vfs_interface_info *)data;

			if (!info)
				return false;

			if (core_path_contains("ppsspp")) {
				log_printf("core", "GET_VFS_INTERFACE skipped for PPSSPP (using native file access)");
				return false;
			}

			if (info->required_interface_version > 3) {
				log_printf("core", "GET_VFS_INTERFACE requested v%u, unsupported",
					info->required_interface_version);
				return false;
			}

			info->required_interface_version = 3;
			info->iface = &vfs_iface;
			log_printf("core", "GET_VFS_INTERFACE -> v3");
			return true;
		}
		case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
			log_printf("core", "GET_INPUT_BITMASKS -> supported");
			return true;
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
			if (!data)
				return false;
			*(const char **)data = core_system_directory();
			log_printf("core", "GET_SYSTEM_DIRECTORY -> %s",
				*(const char **)data ? *(const char **)data : "(null)");
			return true;
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			if (!data)
				return false;
			*(const char **)data = core_save_directory();
			log_printf("core", "GET_SAVE_DIRECTORY -> %s",
				*(const char **)data ? *(const char **)data : "(null)");
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
			if (!data)
				return false;
			*(const char **)data = core_assets_directory();
			log_printf("core", "GET_CORE_ASSETS_DIRECTORY -> %s",
				*(const char **)data ? *(const char **)data : "(null)");
			return true;

		default:
			if ((cmd & 0xFFFF0000u) &&
				(cmd & 0xFFFF0000u) != RETRO_ENVIRONMENT_EXPERIMENTAL &&
				(cmd & 0xFFFF0000u) != RETRO_ENVIRONMENT_PRIVATE) {
				unsigned low_cmd = cmd & 0xFFFFu;

				if (low_cmd != cmd) {
					log_printf("core", "Env alias #%u (0x%X) -> low cmd #%u (0x%X)",
						cmd, cmd, low_cmd, low_cmd);
					return core_environment(low_cmd, data);
				}
			}

			log_printf("core", "Unhandled env #%u (0x%X low=0x%X extra=0x%X)",
				cmd, cmd, cmd & 0xFFFFu, cmd & 0xFFFF0000u);
			return false;
	}

	return true;
}

void input_poll_dummy(void) {}

void core_load(const char *sofile)
{
	void (*set_environment)(retro_environment_t) = NULL;
	void (*set_video_refresh)(retro_video_refresh_t) = NULL;
	void (*set_input_poll)(retro_input_poll_t) = NULL;
	void (*set_input_state)(retro_input_state_t) = NULL;
	void (*set_audio_sample)(retro_audio_sample_t) = NULL;
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = NULL;

	memset(&core, 0, sizeof(core));
	log_printf("core", "loading core library: %s", sofile ? sofile : "(null)");
	core.handle = load_lib(sofile);

	if (!core.handle)
		die("Failed to load core");

	load_retro_sym(retro_init);
	load_retro_sym(retro_deinit);
	load_retro_sym(retro_api_version);
	load_retro_sym(retro_get_system_info);
	load_retro_sym(retro_get_system_av_info);
	load_retro_sym(retro_set_controller_port_device);
	load_retro_sym(retro_reset);
	load_retro_sym(retro_run);
	load_retro_sym(retro_load_game);
	load_retro_sym(retro_unload_game);
	load_retro_sym(retro_get_memory_data);
	load_retro_sym(retro_get_memory_size);

	load_sym(set_environment, retro_set_environment);
	load_sym(set_video_refresh, retro_set_video_refresh);
	load_sym(set_input_poll, retro_set_input_poll);
	load_sym(set_input_state, retro_set_input_state);
	load_sym(set_audio_sample, retro_set_audio_sample);
	load_sym(set_audio_sample_batch, retro_set_audio_sample_batch);

	set_environment(core_environment);
	core.retro_init();
	set_video_refresh(video_refresh);
	set_input_poll(input_poll_dummy);
	set_input_state(input_state);
	set_audio_sample(audio_sample);
	set_audio_sample_batch(audio_sample_batch);

	core.initialized = true;
	log_printf("core", "core loaded successfully");
}

void core_load_game(const char *filename)
{
	struct retro_system_av_info av = {0};
	struct retro_system_info si = {0};
	char resolved_filename[VFS_PATH_MAX];
	const char *content_path = filename;
	struct retro_game_info info = { 0 };
	FILE *file = NULL;
	size_t content_size = 0;

	vfs_resolve_path(resolved_filename, sizeof(resolved_filename), filename);
	if (resolved_filename[0])
		content_path = resolved_filename;

	info.path = content_path;

	core.retro_get_system_info(&si);
	log_printf("core",
		"system info: name='%s' version='%s' need_fullpath=%d block_extract=%d valid_ext='%s'",
		si.library_name ? si.library_name : "(null)",
		si.library_version ? si.library_version : "(null)",
		si.need_fullpath,
		si.block_extract,
		si.valid_extensions ? si.valid_extensions : "(null)");

	if (!si.need_fullpath) {
		file = fopen(content_path, "rb");

		if (!file)
			die("The core could not open the file.");

		fseek(file, 0, SEEK_END);
		content_size = (size_t)ftell(file);
		rewind(file);

		info.size = content_size;
		info.data = malloc(info.size);

		if (!info.data || !fread((void*)info.data, info.size, 1, file))
			die("The core could not read the file.");
		fclose(file);
		file = NULL;
	} else {
		file = fopen(content_path, "rb");
		if (!file)
			die("The core could not open the file.");
		fseek(file, 0, SEEK_END);
		content_size = (size_t)ftell(file);
		fclose(file);
		file = NULL;
		info.data = NULL;
		info.size = 0;
	}

	log_printf("core",
		"loading game: path='%s' buffer_size=%zu file_size=%zu",
		content_path ? content_path : "(null)",
		info.size,
		content_size);
	if (!core.retro_load_game(&info))
		die("The core failed to load the content.");

	core.retro_get_system_av_info(&av);
	log_printf("core",
		"AV info: base=%ux%u max=%ux%u aspect=%.4f fps=%.4f sample_rate=%.2f",
		av.geometry.base_width,
		av.geometry.base_height,
		av.geometry.max_width,
		av.geometry.max_height,
		av.geometry.aspect_ratio,
		av.timing.fps,
		av.timing.sample_rate);
	core_nominal_fps = av.timing.fps > 0.0 ? (float)av.timing.fps : 60.0f;

	video_configure(&av.geometry);
	audio_init(av.timing.sample_rate);

	if (g_cfg.port0) core.retro_set_controller_port_device(0, g_cfg.port0);
	if (g_cfg.port1) core.retro_set_controller_port_device(1, g_cfg.port1);
	if (g_cfg.port2) core.retro_set_controller_port_device(2, g_cfg.port2);
	if (g_cfg.port3) core.retro_set_controller_port_device(3, g_cfg.port3);

	return;
}

void core_run()
{
	core_run_count++;
	if (core_run_count <= 5 || core_run_count % 300 == 0)
		log_printf("core", "core_run #%llu", core_run_count);

	if (runloop_frame_time.callback) {
		retro_time_t current = get_time_usec();
		retro_time_t delta = current - runloop_frame_time_last;

		if (!runloop_frame_time_last)
			delta = runloop_frame_time.reference;
		runloop_frame_time_last = current;
		runloop_frame_time.callback(delta);
	}

	core.retro_run();
}

float core_get_nominal_fps(void)
{
	return core_nominal_fps;
}

size_t core_get_memory_size(unsigned id)
{
	return core.retro_get_memory_size(id);
}

void *core_get_memory_data(unsigned id)
{
	return core.retro_get_memory_data(id);
}

void core_unload()
{
	if (core.initialized)
		core.retro_deinit();

	if (core.handle)
		close_lib(core.handle);
}
