#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <wchar.h>
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

/* OSD message state */
static char core_message_buf[512] = {0};
static int core_message_remaining = 0;

/* Audio callback for MAME and other pull-model cores */
struct retro_audio_callback audio_cb = {0};
bool audio_cb_active = false;
static bool ppsspp_save_root_prepared = false;
static const struct retro_input_descriptor *input_descriptors = NULL;
static const struct retro_controller_info *controller_infos = NULL;
static const struct retro_subsystem_info *subsystem_infos = NULL;
static struct retro_disk_control_callback disk_control_cb = {0};
static struct retro_disk_control_ext_callback disk_control_ext_cb = {0};
static struct retro_fastforwarding_override fastforward_override = {0};
static struct retro_memory_map memory_map = {0};
static struct retro_core_options_update_display_callback options_display_cb = {0};
extern config g_cfg;

#define VFS_PATH_MAX 4096

static char core_system_dir[VFS_PATH_MAX] = {0};
static char core_save_dir[VFS_PATH_MAX] = {0};
static char core_memstick_dir[VFS_PATH_MAX] = {0};
static bool core_dirs_prepared = false;

static char qemu_cmd_line_sanitized_path[VFS_PATH_MAX] = {0};

static const char *message_target_name(enum retro_message_target target)
{
	switch (target) {
		case RETRO_MESSAGE_TARGET_ALL:
			return "all";
		case RETRO_MESSAGE_TARGET_OSD:
			return "osd";
		case RETRO_MESSAGE_TARGET_LOG:
			return "log";
		default:
			return "unknown";
	}
}

static const char *message_type_name(enum retro_message_type type)
{
	switch (type) {
		case RETRO_MESSAGE_TYPE_NOTIFICATION:
			return "notification";
		case RETRO_MESSAGE_TYPE_NOTIFICATION_ALT:
			return "notification_alt";
		case RETRO_MESSAGE_TYPE_STATUS:
			return "status";
		case RETRO_MESSAGE_TYPE_PROGRESS:
			return "progress";
		default:
			return "unknown";
	}
}

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
	WIN32_FIND_DATAW data;
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

#if defined(_WIN32)
static bool vfs_utf8_to_wide(const char *src, wchar_t *dst, size_t dst_count)
{
	int needed = 0;

	if (!src || !dst || dst_count == 0)
		return false;

	needed = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)dst_count);
	if (needed > 0)
		return true;

	needed = MultiByteToWideChar(CP_ACP, 0, src, -1, dst, (int)dst_count);
	return needed > 0;
}

static bool vfs_wide_to_utf8(const wchar_t *src, char *dst, size_t dst_size)
{
	int needed = 0;

	if (!src || !dst || dst_size == 0)
		return false;

	needed = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)dst_size, NULL, NULL);
	return needed > 0;
}

static FILE *vfs_fopen(const char *path, const char *mode)
{
	wchar_t wide_path[VFS_PATH_MAX];
	wchar_t wide_mode[16];

	if (!path || !mode)
		return NULL;

	if (!vfs_utf8_to_wide(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])))
		return NULL;
	if (!vfs_utf8_to_wide(mode, wide_mode, sizeof(wide_mode) / sizeof(wide_mode[0])))
		return NULL;

	return _wfopen(wide_path, wide_mode);
}

static int vfs_remove_utf8(const char *path)
{
	wchar_t wide_path[VFS_PATH_MAX];

	if (!path)
		return -1;
	if (!vfs_utf8_to_wide(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])))
		return -1;

	return _wremove(wide_path);
}

static int vfs_rename_utf8(const char *old_path, const char *new_path)
{
	wchar_t wide_old[VFS_PATH_MAX];
	wchar_t wide_new[VFS_PATH_MAX];

	if (!old_path || !new_path)
		return -1;
	if (!vfs_utf8_to_wide(old_path, wide_old, sizeof(wide_old) / sizeof(wide_old[0])) ||
		!vfs_utf8_to_wide(new_path, wide_new, sizeof(wide_new) / sizeof(wide_new[0])))
		return -1;

	return _wrename(wide_old, wide_new);
}
#else
static FILE *vfs_fopen(const char *path, const char *mode)
{
	return fopen(path, mode);
}

static int vfs_remove_utf8(const char *path)
{
	return remove(path);
}

static int vfs_rename_utf8(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}
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
		strcmp(key, "mupen64plus-EnableFBEmulation") == 0 ||
		strcmp(key, "puae_use_whdload") == 0 ||
		strcmp(key, "puae_use_whdload_prefs") == 0 ||
		strcmp(key, "puae_use_boot_hd") == 0 ||
		strcmp(key, "puae_model_hd") == 0;
}

static bool core_path_contains(const char *needle);
static bool path_has_suffix_ci(const char *path, const char *suffix);
static const char *prepare_qemu_cmd_line_path(const char *content_path);

static bool core_prefers_vulkan_hw(void)
{
	return g_cfg.core != NULL;
}

static bool core_path_contains(const char *needle)
{
	return g_cfg.core && needle && strstr(g_cfg.core, needle) != NULL;
}

static bool path_has_suffix_ci(const char *path, const char *suffix)
{
	size_t path_len = 0;
	size_t suffix_len = 0;

	if (!path || !suffix)
		return false;

	path_len = strlen(path);
	suffix_len = strlen(suffix);
	if (path_len < suffix_len)
		return false;

#if defined(_WIN32)
	return _stricmp(path + path_len - suffix_len, suffix) == 0;
#else
	return strcasecmp(path + path_len - suffix_len, suffix) == 0;
#endif
}

static const char *prepare_qemu_cmd_line_path(const char *content_path)
{
	FILE *src = NULL;
	FILE *dst = NULL;
	char *buffer = NULL;
	long file_size = 0;
	size_t read_size = 0;
	size_t trimmed_size = 0;
	const char *last_sep = NULL;
	size_t dir_len = 0;
	static const char *sanitized_name = "__rombundler_qemu_cmd_line.qemu_cmd_line";

	qemu_cmd_line_sanitized_path[0] = '\0';

	if (!content_path)
		return content_path;
	if (!core_path_contains("qemu"))
		return content_path;
	if (!path_has_suffix_ci(content_path, ".qemu_cmd_line"))
		return content_path;

	src = vfs_fopen(content_path, "rb");
	if (!src)
		return content_path;

	if (fseek(src, 0, SEEK_END) != 0) {
		fclose(src);
		return content_path;
	}

	file_size = ftell(src);
	if (file_size < 0) {
		fclose(src);
		return content_path;
	}

	rewind(src);
	buffer = (char *)malloc((size_t)file_size + 1);
	if (!buffer) {
		fclose(src);
		return content_path;
	}

	read_size = fread(buffer, 1, (size_t)file_size, src);
	fclose(src);
	src = NULL;

	buffer[read_size] = '\0';
	trimmed_size = read_size;
	while (trimmed_size > 0) {
		char c = buffer[trimmed_size - 1];
		if (c != '\r' && c != '\n' && c != '\t' && c != ' ')
			break;
		trimmed_size--;
	}

	if (trimmed_size == read_size) {
		free(buffer);
		return content_path;
	}

	last_sep = strrchr(content_path, '\\');
	if (!last_sep)
		last_sep = strrchr(content_path, '/');
	dir_len = last_sep ? (size_t)(last_sep - content_path + 1) : 0;

	if (dir_len + strlen(sanitized_name) + 1 >= sizeof(qemu_cmd_line_sanitized_path)) {
		free(buffer);
		return content_path;
	}

	if (dir_len > 0)
		memcpy(qemu_cmd_line_sanitized_path, content_path, dir_len);
	strcpy(qemu_cmd_line_sanitized_path + dir_len, sanitized_name);

	dst = vfs_fopen(qemu_cmd_line_sanitized_path, "wb");
	if (!dst) {
		qemu_cmd_line_sanitized_path[0] = '\0';
		free(buffer);
		return content_path;
	}

	if (trimmed_size > 0)
		fwrite(buffer, 1, trimmed_size, dst);
	fclose(dst);
	free(buffer);

	log_printf("core",
		"sanitized qemu command file original='%s' sanitized='%s' size=%zu trimmed=%zu",
		content_path,
		qemu_cmd_line_sanitized_path,
		read_size,
		trimmed_size);

	return qemu_cmd_line_sanitized_path;
}

static bool ppsspp_serial_prefix_match(const char *s)
{
	static const char *prefixes[] = {
		"UCUS", "UCES", "UCJS",
		"ULUS", "ULES", "ULJS", "ULJM",
		"NPUH", "NPUG", "NPUZ", "NPJH", "NPJG", "NPJJ", "NPEG",
	};
	size_t i = 0;

	if (!s)
		return false;

	for (i = 0; i < (sizeof(prefixes) / sizeof(prefixes[0])); i++) {
		if (strncmp(s, prefixes[i], 4) == 0)
			return true;
	}

	return false;
}

static bool ppsspp_is_serial_candidate(const char *s)
{
	size_t i = 0;

	if (!s)
		return false;
	if (!ppsspp_serial_prefix_match(s))
		return false;

	for (i = 0; i < 4; i++) {
		if (s[i] < 'A' || s[i] > 'Z')
			return false;
	}

	for (i = 4; i < 9; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
	}

	return true;
}

static bool ppsspp_serial_exists(char serials[][10], size_t count, const char *candidate)
{
	size_t i = 0;

	for (i = 0; i < count; i++) {
		if (strncmp(serials[i], candidate, 9) == 0)
			return true;
	}

	return false;
}

static size_t ppsspp_collect_serials_from_content(const char *content_path, char serials[][10], size_t max_serials)
{
	FILE *file = NULL;
	unsigned char chunk[4096];
	unsigned char window[9] = {0};
	size_t count = 0;
	size_t have = 0;
	size_t read_bytes = 0;
	size_t i = 0;

	if (!content_path || !serials || max_serials == 0)
		return 0;

	file = vfs_fopen(content_path, "rb");
	if (!file)
		return 0;

	while ((read_bytes = fread(chunk, 1, sizeof(chunk), file)) > 0 && count < max_serials) {
		for (i = 0; i < read_bytes; i++) {
			if (have < sizeof(window)) {
				window[have++] = chunk[i];
			} else {
				memmove(window, window + 1, sizeof(window) - 1);
				window[sizeof(window) - 1] = chunk[i];
			}

			if (have == sizeof(window)) {
				char candidate[10];
				memcpy(candidate, window, 9);
				candidate[9] = '\0';

				if (ppsspp_is_serial_candidate(candidate) &&
				    !ppsspp_serial_exists(serials, count, candidate)) {
					snprintf(serials[count], 10, "%s", candidate);
					count++;
					if (count >= max_serials)
						break;
				}
			}
		}
	}

	fclose(file);
	return count;
}

static bool ensure_directory_exists(const char *path)
{
	int rc = 0;

	if (!path || !*path)
		return false;

#if defined(_WIN32)
	{
		wchar_t wide_path[VFS_PATH_MAX];

		if (!vfs_utf8_to_wide(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])))
			return false;
		rc = _wmkdir(wide_path);
	}
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
		wchar_t wide_normalized[VFS_PATH_MAX];
		wchar_t wide_resolved[VFS_PATH_MAX];
		DWORD rc = 0;

		if (vfs_utf8_to_wide(normalized,
			wide_normalized,
			sizeof(wide_normalized) / sizeof(wide_normalized[0]))) {
			rc = GetFullPathNameW(wide_normalized,
				(DWORD)(sizeof(wide_resolved) / sizeof(wide_resolved[0])),
				wide_resolved,
				NULL);
			if (rc > 0 &&
				rc < (sizeof(wide_resolved) / sizeof(wide_resolved[0])) &&
				vfs_wide_to_utf8(wide_resolved, dst, dst_size))
				return;
		}
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

static bool vfs_is_content_image_path(const char *path)
{
	const char *ext = NULL;

	if (!path)
		return false;

	ext = strrchr(path, '.');
	if (!ext)
		return false;

#if defined(_WIN32)
	return _stricmp(ext, ".iso") == 0 ||
		_stricmp(ext, ".cso") == 0 ||
		_stricmp(ext, ".chd") == 0 ||
		_stricmp(ext, ".pbp") == 0 ||
		_stricmp(ext, ".elf") == 0 ||
		_stricmp(ext, ".prx") == 0;
#else
	return strcasecmp(ext, ".iso") == 0 ||
		strcasecmp(ext, ".cso") == 0 ||
		strcasecmp(ext, ".chd") == 0 ||
		strcasecmp(ext, ".pbp") == 0 ||
		strcasecmp(ext, ".elf") == 0 ||
		strcasecmp(ext, ".prx") == 0;
#endif
}

static bool vfs_get_file_size_by_path(const char *path, int64_t *size_out, bool *is_dir_out)
{
	char native_path[VFS_PATH_MAX];

	if (!path)
		return false;

	vfs_resolve_path(native_path, sizeof(native_path), path);

#if defined(_WIN32)
	{
		wchar_t wide_path[VFS_PATH_MAX];
		WIN32_FILE_ATTRIBUTE_DATA data;
		LARGE_INTEGER file_size;

		if (!vfs_utf8_to_wide(native_path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])))
			return false;

		if (!GetFileAttributesExW(wide_path, GetFileExInfoStandard, &data))
			return false;

		file_size.HighPart = (LONG)data.nFileSizeHigh;
		file_size.LowPart = data.nFileSizeLow;

		if (size_out)
			*size_out = file_size.QuadPart;
		if (is_dir_out)
			*is_dir_out = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		return true;
	}
#else
	{
		struct stat st;
		if (stat(native_path, &st) != 0)
			return false;
		if (size_out)
			*size_out = (int64_t)st.st_size;
		if (is_dir_out)
			*is_dir_out = S_ISDIR(st.st_mode);
		return true;
	}
#endif
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
	{
		wchar_t wide_path[VFS_PATH_MAX];

		if (!vfs_utf8_to_wide(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])))
			return -1;
		rc = _wmkdir(wide_path);
	}
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
	stream->fp = vfs_fopen(native_path, fmode);

	if (core_path_contains("ppsspp") && vfs_is_content_image_path(native_path))
		log_printf("core", "VFS open path='%s' mode=0x%x hints=0x%x fmode='%s'",
			native_path, mode, hints, fmode);

	if (!stream->fp && vfs_is_write_mode(mode)) {
		char parent[VFS_PATH_MAX];
		if (vfs_extract_parent_path(native_path, parent, sizeof(parent)))
			vfs_mkdir_recursive(parent);
		stream->fp = vfs_fopen(native_path, fmode);
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

	if (vfs_get_file_size_by_path(stream->path, &size, NULL)) {
		if (core_path_contains("ppsspp") && vfs_is_content_image_path(stream->path))
			log_printf("core", "VFS size(path) path='%s' -> %lld",
				stream->path, (long long)size);
		return size;
	}

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

	if (core_path_contains("ppsspp") && vfs_is_content_image_path(stream->path))
		log_printf("core", "VFS size(stream) path='%s' -> %lld",
			stream->path, (long long)size);

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
	return vfs_remove_utf8(native_path) == 0 ? 0 : -1;
}

static int retro_vfs_file_rename_impl(const char *old_path, const char *new_path)
{
	char old_native[VFS_PATH_MAX];
	char new_native[VFS_PATH_MAX];

	if (!old_path || !new_path)
		return -1;

	vfs_resolve_path(old_native, sizeof(old_native), old_path);
	vfs_resolve_path(new_native, sizeof(new_native), new_path);
	return vfs_rename_utf8(old_native, new_native) == 0 ? 0 : -1;
}

static int retro_vfs_stat_impl(const char *path, int32_t *size)
{
	int64_t file_size = 0;
	bool is_dir = false;

	if (!path)
		return 0;

	if (!vfs_get_file_size_by_path(path, &file_size, &is_dir))
		return 0;

	if (size)
		*size = (int32_t)file_size;

	if (core_path_contains("ppsspp") && vfs_is_content_image_path(path))
		log_printf("core", "VFS stat path='%s' -> size=%lld is_dir=%d",
			path, (long long)file_size, is_dir ? 1 : 0);
	return RETRO_VFS_STAT_IS_VALID | (is_dir ? RETRO_VFS_STAT_IS_DIRECTORY : 0);
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
		wchar_t wide_pattern[VFS_PATH_MAX];
		size_t len = 0;

		snprintf(native_path, sizeof(native_path), "%s", dirstream->dirpath);
		snprintf(pattern, sizeof(pattern), "%s", native_path);
		len = strlen(pattern);
		if (len > 0 && pattern[len - 1] != '\\' && pattern[len - 1] != '/')
			snprintf(pattern + len, sizeof(pattern) - len, "\\*");
		else
			snprintf(pattern + len, sizeof(pattern) - len, "*");

		if (!vfs_utf8_to_wide(pattern, wide_pattern, sizeof(wide_pattern) / sizeof(wide_pattern[0]))) {
			free(dirstream);
			return NULL;
		}

		dirstream->handle = FindFirstFileW(wide_pattern, &dirstream->data);
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
		WIN32_FIND_DATAW *data = &dirstream->data;
		char utf8_name[VFS_PATH_MAX];
		if (dirstream->first)
			dirstream->first = false;
		else if (!FindNextFileW(dirstream->handle, data))
			return false;

		if (!vfs_wide_to_utf8(data->cFileName, utf8_name, sizeof(utf8_name)))
			continue;

		if (!dirstream->include_hidden &&
			((data->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) || utf8_name[0] == '.'))
			continue;

		snprintf(dirstream->name, sizeof(dirstream->name), "%s", utf8_name);
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
		"./memstick",
		"./memstick/PSP",
		"./memstick/PSP/SAVEDATA",
		"./memstick/PSP/SYSTEM",
		"./memstick/PSP/SYSTEM/CACHE",
		"./memstick/PSP/Cheats",
		"./memstick/PSP/GAME",
		"./memstick/PSP/TEXTURES",
		"./memstick/PSP/PPSSPP_STATE",
		"./memstick/flash0",
	};
	bool ok = true;
	size_t i = 0;

	for (i = 0; i < (sizeof(paths) / sizeof(paths[0])); i++) {
		if (!ensure_directory_exists(paths[i]))
			ok = false;
	}

	return ok;
}

static void prepare_ppsspp_game_save_dirs(const char *content_path)
{
	char serials[8][10] = {{0}};
	size_t count = 0;
	size_t i = 0;
	static const char *suffixes[] = {
		"",
		"Profile",
	};

	if (!core_path_contains("ppsspp"))
		return;

	if (!prepare_ppsspp_save_root())
		return;

	count = ppsspp_collect_serials_from_content(content_path, serials, 8);
	if (count == 0) {
		log_printf("core", "PPSSPP save dirs: no serial found in '%s'",
			content_path ? content_path : "(null)");
		return;
	}

	for (i = 0; i < count; i++) {
		size_t j = 0;
		log_printf("core", "PPSSPP content serial detected: %s", serials[i]);
		for (j = 0; j < (sizeof(suffixes) / sizeof(suffixes[0])); j++) {
			char save_dir[VFS_PATH_MAX];
			snprintf(save_dir, sizeof(save_dir), "./memstick/PSP/SAVEDATA/%s%s",
				serials[i], suffixes[j]);
			if (ensure_directory_exists(save_dir))
				log_printf("core", "PPSSPP save dir prepared: %s", save_dir);
		}
	}
}

static void prepare_core_directories(void)
{
	if (core_dirs_prepared)
		return;

	vfs_resolve_path(core_system_dir, sizeof(core_system_dir), "./system");
	vfs_resolve_path(core_save_dir, sizeof(core_save_dir), "./saves");
	vfs_resolve_path(core_memstick_dir, sizeof(core_memstick_dir), "./memstick");

	if (core_system_dir[0])
		ensure_directory_exists(core_system_dir);

	if (core_save_dir[0])
		ensure_directory_exists(core_save_dir);

	core_dirs_prepared = true;
}

static const char *core_system_directory(void)
{
	prepare_core_directories();
	return core_system_dir[0] ? core_system_dir : "./system";
}

static const char *core_save_directory(void)
{
	prepare_core_directories();

	if (core_path_contains("ppsspp")) {
		if (!ppsspp_save_root_prepared) {
			bool created = prepare_ppsspp_save_root();
			log_printf("core", "PPSSPP save root '%s' prepared=%s (memstick layout)",
				core_memstick_dir[0] ? core_memstick_dir : "./memstick",
				created ? "yes" : "no");
			ppsspp_save_root_prepared = true;
		}
		return core_memstick_dir[0] ? core_memstick_dir : "./memstick";
	}

	return core_save_dir[0] ? core_save_dir : "./saves";
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

static void core_set_led_state(int led, int state)
{
	(void)led;
	(void)state;
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
		case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: {
			*(unsigned *)data = 1;
			return true;
		}
		case RETRO_ENVIRONMENT_SHUTDOWN: {
			log_printf("core", "RETRO_ENVIRONMENT_SHUTDOWN requested by core");
			video_should_close(1);
		}
		break;
		case RETRO_ENVIRONMENT_SET_MESSAGE: {
			const struct retro_message *msg = (const struct retro_message *)data;
			if (msg && msg->msg) {
				log_printf("core", "SET_MESSAGE frames=%u text='%s'",
					msg->frames, msg->msg);
				strncpy(core_message_buf, msg->msg, sizeof(core_message_buf) - 1);
				core_message_buf[sizeof(core_message_buf) - 1] = '\0';
				core_message_remaining = (int)msg->frames;
			}
			return true;
		}
		case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
			*(int*)data = 1 << 0 | 1 << 1;
		}
		break;
		case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
			const struct retro_frame_time_callback *cb =
				(const struct retro_frame_time_callback*)data;
			if (cb) {
				runloop_frame_time = *cb;
			} else {
				memset(&runloop_frame_time, 0, sizeof(runloop_frame_time));
				runloop_frame_time_last = 0;
			}
		}
		break;
		case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
			const struct retro_keyboard_callback *cb =
				(const struct retro_keyboard_callback*)data;
			input_set_keyboard_callback(cb ? cb->callback : NULL);
		}
		break;
		case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
			log_printf("core", "SET_SUPPORT_NO_GAME accepted");
			return true;
		case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
			if (!data)
				return false;
			*(unsigned *)data = 1;
			log_printf("core", "GET_DISK_CONTROL_INTERFACE_VERSION -> 1");
			return true;
		case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
			if (!data)
				return false;
			disk_control_cb = *(const struct retro_disk_control_callback *)data;
			log_printf("core", "SET_DISK_CONTROL_INTERFACE accepted");
			return true;
		case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
			if (!data)
				return false;
			disk_control_ext_cb = *(const struct retro_disk_control_ext_callback *)data;
			log_printf("core", "SET_DISK_CONTROL_EXT_INTERFACE accepted");
			return true;
		case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
			log_printf("core", "SET_SUPPORT_ACHIEVEMENTS accepted");
			return true;
		case RETRO_ENVIRONMENT_GET_LED_INTERFACE: {
			struct retro_led_interface *iface = (struct retro_led_interface *)data;
			if (!iface)
				return false;
			iface->set_led_state = core_set_led_state;
			log_printf("core", "GET_LED_INTERFACE -> stub");
			return true;
		}
		case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
			if (!data)
				return false;
			fastforward_override = *(const struct retro_fastforwarding_override *)data;
			log_printf("core", "SET_FASTFORWARDING_OVERRIDE accepted ratio=%.3f fastforward=%d notification=%d",
				fastforward_override.ratio,
				fastforward_override.fastforward ? 1 : 0,
				fastforward_override.notification ? 1 : 0);
			return true;
		case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
			struct retro_perf_callback *perf_cb = (struct retro_perf_callback*)data;
			if (!perf_cb) return false;
			perf_cb->get_time_usec = get_time_usec;
			perf_cb->get_cpu_features = NULL;
			perf_cb->get_perf_counter = NULL;
			perf_cb->perf_register = NULL;
			perf_cb->perf_start = NULL;
			perf_cb->perf_stop = NULL;
			perf_cb->perf_log = NULL;
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
		case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
			const struct retro_message_ext *msg = (const struct retro_message_ext *)data;
			if (msg && msg->msg) {
				log_printf("core",
					"SET_MESSAGE_EXT target=%s type=%s level=%d duration=%u priority=%u progress=%d text='%s'",
					message_target_name(msg->target),
					message_type_name(msg->type),
					(int)msg->level,
					msg->duration,
					msg->priority,
					msg->progress,
					msg->msg);
				if (msg->duration > 0) {
					strncpy(core_message_buf, msg->msg, sizeof(core_message_buf) - 1);
					core_message_buf[sizeof(core_message_buf) - 1] = '\0';
					core_message_remaining = (int)msg->duration;
				}
			}
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
			if (!var || !var->key) return false;
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
				return false;

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

			if (core_path_contains("pcsx2")) {
				log_printf("core", "GET_VFS_INTERFACE skipped for PCSX2 (using native file access)");
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
		case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
			if (!data)
				return false;
			*(const char **)data = g_cfg.core ? g_cfg.core : "";
			return true;

		case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
			log_printf("core", "SET_SERIALIZATION_QUIRKS accepted value=%llu",
				data ? (unsigned long long)*(uint64_t*)data : 0ULL);
			return true;
		case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK: {
			const struct retro_audio_callback *cb =
				(const struct retro_audio_callback *)data;
			if (!cb) return false;
			audio_cb = *cb;
			audio_cb_active = (cb->callback != NULL);
			log_printf("core", "SET_AUDIO_CALLBACK registered callback=%p",
				(void*)(cb->callback));
			return true;
		}
		case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK:
			if (!data) return false;
			log_printf("core", "SET_PROC_ADDRESS_CALLBACK accepted");
			return true;
		case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
			if (!data) return false;
			*(bool*)data = false;
			log_printf("core", "GET_JIT_CAPABLE -> false");
			return true;
		case RETRO_ENVIRONMENT_SET_ROTATION: {
			if (!data) return false;
			unsigned rotation = *(const unsigned *)data;
			log_printf("core", "SET_ROTATION %u", rotation);
			return true;
		}
		case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
			if (!data) return false;
			*(unsigned *)data = 5;
			return true;
		case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
			log_printf("core", "SET_PERFORMANCE_LEVEL value=%u",
				data ? *(const unsigned *)data : 0);
			return true;
		case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
			if (!data) return false;
			*(float *)data = core_nominal_fps;
			return true;
		case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
			if (!data) return false;
			*(uint64_t*)data = (1ULL << RETRO_DEVICE_JOYPAD) |
			                  (1ULL << RETRO_DEVICE_MOUSE) |
			                  (1ULL << RETRO_DEVICE_KEYBOARD) |
			                  (1ULL << RETRO_DEVICE_ANALOG);
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

void core_load(const char *sofile)
{
	void (*set_environment)(retro_environment_t) = NULL;
	void (*set_video_refresh)(retro_video_refresh_t) = NULL;
	void (*set_input_poll)(retro_input_poll_t) = NULL;
	void (*set_input_state)(retro_input_state_t) = NULL;
	void (*set_audio_sample)(retro_audio_sample_t) = NULL;
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = NULL;
	int options_preload_result = 0;

	memset(&core, 0, sizeof(core));
	log_printf("core", "loading core library: %s", sofile ? sofile : "(null)");
	options_preload_result = opt_load("./options.ini");
	if (options_preload_result < 0)
		log_printf("core", "options.ini preload unavailable before retro_init");
	else
		log_printf("core", "options.ini preloaded before retro_init");
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
	set_input_poll(input_poll);
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
	content_path = prepare_qemu_cmd_line_path(content_path);

	info.path = content_path;
	prepare_ppsspp_game_save_dirs(content_path);

	core.retro_get_system_info(&si);
	log_printf("core",
		"system info: name='%s' version='%s' need_fullpath=%d block_extract=%d valid_ext='%s'",
		si.library_name ? si.library_name : "(null)",
		si.library_version ? si.library_version : "(null)",
		si.need_fullpath,
		si.block_extract,
		si.valid_extensions ? si.valid_extensions : "(null)");

	if (!si.need_fullpath) {
		file = vfs_fopen(content_path, "rb");

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
		file = vfs_fopen(content_path, "rb");
		if (!file)
			die("The core could not open the file.");
		fseek(file, 0, SEEK_END);
		content_size = (size_t)ftell(file);
		fclose(file);
		file = NULL;
		info.data = NULL;
		info.size = core_path_contains("ppsspp") ? 0 : content_size;
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

	/* Ativa áudio via callback (MAME etc) após dispositivo de áudio estar pronto */
	if (audio_cb_active && audio_cb.set_state) {
		audio_cb.set_state(true);
		audio_start_thread();
	}

	core.retro_set_controller_port_device(0, g_cfg.port0 ? g_cfg.port0 : RETRO_DEVICE_JOYPAD);
	core.retro_set_controller_port_device(1, g_cfg.port1 ? g_cfg.port1 : RETRO_DEVICE_JOYPAD);
	core.retro_set_controller_port_device(2, g_cfg.port2 ? g_cfg.port2 : RETRO_DEVICE_JOYPAD);
	core.retro_set_controller_port_device(3, g_cfg.port3 ? g_cfg.port3 : RETRO_DEVICE_JOYPAD);

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

	/* Audio pull is now handled by the dedicated audio thread.
	 * No synchronous audio_callback_pull() needed here. */

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

void core_message_update(void)
{
	if (core_message_remaining > 0)
		core_message_remaining--;
}

const char *core_message_text(void)
{
	if (core_message_remaining <= 0 || core_message_buf[0] == '\0')
		return NULL;
	return core_message_buf;
}

int core_message_frames(void)
{
	return core_message_remaining;
}

void core_unload()
{
	/* Stop audio thread before unloading — the callback pointer
	 * belongs to the core library and must not be called after
	 * retro_deinit / dlclose. */
	audio_stop_thread();

	if (core.initialized)
		core.retro_deinit();

	if (core.handle)
		close_lib(core.handle);

	if (qemu_cmd_line_sanitized_path[0]) {
		vfs_remove_utf8(qemu_cmd_line_sanitized_path);
		qemu_cmd_line_sanitized_path[0] = '\0';
	}
}
