#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <time.h>

static char g_log_path[1024] = {0};
static int g_log_ready = 0;

void log_printf(const char *tag, const char *fmt, ...);

static void log_timestamp(FILE *stream)
{
	time_t now = time(NULL);
	struct tm local_tm;

#if defined(_WIN32)
	localtime_s(&local_tm, &now);
#else
	localtime_r(&now, &local_tm);
#endif

	fprintf(stream, "%04d-%02d-%02d %02d:%02d:%02d",
		local_tm.tm_year + 1900,
		local_tm.tm_mon + 1,
		local_tm.tm_mday,
		local_tm.tm_hour,
		local_tm.tm_min,
		local_tm.tm_sec);
}

void log_init(const char *path)
{
	const char *target = path && *path ? path : "./rombundler.log";

	strncpy(g_log_path, target, sizeof(g_log_path) - 1);

	if (!freopen(g_log_path, "w", stdout))
		return;

	if (!freopen(g_log_path, "a", stderr))
		return;

	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);
	g_log_ready = 1;

	log_printf("app", "logging initialized at %s", g_log_path);
}

void log_printf(const char *tag, const char *fmt, ...)
{
	FILE *stream = stderr ? stderr : stdout;
	va_list va;

	if (!stream)
		return;

	log_timestamp(stream);
	fprintf(stream, " [%s] ", tag ? tag : "log");

	va_start(va, fmt);
	vfprintf(stream, fmt, va);
	va_end(va);

	fputc('\n', stream);
	fflush(stream);
}

const char *log_get_path(void)
{
	return g_log_ready ? g_log_path : NULL;
}

void die(const char *fmt, ...) {
	char buffer[4096];

	va_list va;
	va_start(va, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, va);
	va_end(va);

	if (g_log_ready)
		log_printf("fatal", "%s", buffer);
	else {
		fputs(buffer, stderr);
		fputc('\n', stderr);
		fflush(stderr);
	}

	exit(EXIT_FAILURE);
}
