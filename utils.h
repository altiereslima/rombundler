void log_init(const char *path);
void log_printf(const char *tag, const char *fmt, ...);
const char *log_get_path(void);
void die(const char *fmt, ...);
