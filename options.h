#ifndef OPTIONS_H
#define OPTIONS_H

#include <stdbool.h>

#define OPT_MAX_OPTIONS   128
#define OPT_MAX_VALUES    128
#define OPT_MAX_KEY_LEN   128
#define OPT_MAX_DESC_LEN  256
#define OPT_MAX_VAL_LEN   128
#define OPT_MAX_LABEL_LEN 256

typedef struct {
    char key[OPT_MAX_KEY_LEN];
    char desc[OPT_MAX_DESC_LEN];        /* human-readable description */
    char values[OPT_MAX_VALUES][OPT_MAX_VAL_LEN];   /* raw values sent to core */
    char labels[OPT_MAX_VALUES][OPT_MAX_LABEL_LEN];  /* display labels (may differ) */
    int  num_values;
    int  selected;
    bool visible;
} core_option_t;

/* v0: parse retro_variable array ("Desc; val1|val2|val3") */
void opt_parse_variables(const void *data);

/* v1: parse retro_core_option_definition array */
void opt_parse_v1(const void *data);

/* v1 intl: parse retro_core_options_intl (with localized variant) */
void opt_parse_v1_intl(const void *data);

/* v2: parse retro_core_options_v2 / v2_intl */
void opt_parse_v2(const void *data);
void opt_parse_v2_intl(const void *data);

bool get_option(const char *key, const char **value);
int  opt_handler(void *user, const char *section, const char *name, const char *value);
int  opt_load(const char *path);
int  opt_save(const char *path);
bool opt_override(const char *key, const char *value);
bool opt_has_value(const char *key, const char *value);
bool opt_override_first_available(const char *key, const char *const *values, int count, const char **selected_value);
bool opt_value_allowed(const char *key, const char *value);

bool opt_updated(void);
void opt_clear_updated(void);
bool opt_set_visible(const char *key, bool visible);

int  opt_count(void);
const core_option_t *opt_get_entry(int index);
void opt_cycle(int index, int dir);

#endif
