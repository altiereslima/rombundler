/* ---------------------------------------------------------------
 * options.c -- Core options: v0 (SET_VARIABLES) and v1
 *              (SET_CORE_OPTIONS / SET_CORE_OPTIONS_INTL).
 * --------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "options.h"
#include "ini.h"
#include "libretro.h"

static core_option_t options[OPT_MAX_OPTIONS];
static int           num_options = 0;
static bool          vars_updated = false;

typedef struct {
    char key[OPT_MAX_KEY_LEN];
    char value[OPT_MAX_VAL_LEN];
} option_override_t;

static option_override_t pending_overrides[OPT_MAX_OPTIONS];
static int               num_pending_overrides = 0;

static void normalize_option_value(const char *value, char *out, size_t out_size)
{
    const char *start = value ? value : "";
    size_t len = 0;

    if (!out || out_size == 0)
        return;

    while (*start && isspace((unsigned char)*start))
        start++;

    len = strlen(start);
    while (len > 0 && isspace((unsigned char)start[len - 1]))
        len--;

    if (len >= 2) {
        char quote = start[0];
        if ((quote == '"' || quote == '\'') && start[len - 1] == quote) {
            start++;
            len -= 2;

            while (len > 0 && isspace((unsigned char)*start)) {
                start++;
                len--;
            }

            while (len > 0 && isspace((unsigned char)start[len - 1]))
                len--;
        }
    }

    if (len >= out_size)
        len = out_size - 1;

    memcpy(out, start, len);
    out[len] = '\0';
}

static void write_quoted_option_value(FILE *f, const char *value)
{
    const char *p = value ? value : "";

    fputc('"', f);
    while (*p) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        fputc(*p, f);
        p++;
    }
    fputc('"', f);
}

static int find_option_index(const char *key)
{
    for (int i = 0; i < num_options; i++) {
        if (strcmp(options[i].key, key) == 0)
            return i;
    }
    return -1;
}

static void remember_override(const char *key, const char *value)
{
    char normalized[OPT_MAX_VAL_LEN];

    normalize_option_value(value, normalized, sizeof(normalized));

    for (int i = 0; i < num_pending_overrides; i++) {
        if (strcmp(pending_overrides[i].key, key) == 0) {
            strncpy(pending_overrides[i].value, normalized, OPT_MAX_VAL_LEN - 1);
            pending_overrides[i].value[OPT_MAX_VAL_LEN - 1] = '\0';
            return;
        }
    }

    if (num_pending_overrides >= OPT_MAX_OPTIONS)
        return;

    memset(&pending_overrides[num_pending_overrides], 0, sizeof(pending_overrides[0]));
    strncpy(pending_overrides[num_pending_overrides].key, key, OPT_MAX_KEY_LEN - 1);
    strncpy(pending_overrides[num_pending_overrides].value, normalized, OPT_MAX_VAL_LEN - 1);
    pending_overrides[num_pending_overrides].key[OPT_MAX_KEY_LEN - 1] = '\0';
    pending_overrides[num_pending_overrides].value[OPT_MAX_VAL_LEN - 1] = '\0';
    num_pending_overrides++;
}

static void apply_override(const char *key, const char *value)
{
    int option_index = find_option_index(key);
    char normalized[OPT_MAX_VAL_LEN];

    normalize_option_value(value, normalized, sizeof(normalized));

    if (option_index < 0)
        return;

    for (int i = 0; i < options[option_index].num_values; i++) {
        if (strcmp(options[option_index].values[i], normalized) == 0) {
            options[option_index].selected = i;
            return;
        }
    }
}

static void apply_pending_overrides(void)
{
    for (int i = 0; i < num_pending_overrides; i++)
        apply_override(pending_overrides[i].key, pending_overrides[i].value);
}

static const char *find_pending_override_value(const char *key)
{
    if (!key || !key[0])
        return NULL;

    for (int i = 0; i < num_pending_overrides; i++) {
        if (strcmp(pending_overrides[i].key, key) == 0)
            return pending_overrides[i].value;
    }

    return NULL;
}

static bool is_option_value_allowed(const char *key, const char *value)
{
    char normalized[OPT_MAX_VAL_LEN];

    normalize_option_value(value, normalized, sizeof(normalized));

    if (!key || !value || !key[0] || !value[0])
        return false;

    return normalized[0] != '\0';
}

/* ── v0: parse SET_VARIABLES ── */

void opt_parse_variables(const void *data)
{
    const struct retro_variable *vars = (const struct retro_variable *)data;
    if (!vars) return;

    num_options = 0;

    for (; vars->key != NULL && num_options < OPT_MAX_OPTIONS; vars++) {
        core_option_t *o = &options[num_options];
        memset(o, 0, sizeof(*o));
        strncpy(o->key, vars->key, OPT_MAX_KEY_LEN - 1);
        o->visible = true;

        if (!vars->value) { num_options++; continue; }

        const char *semi = strchr(vars->value, ';');
        if (semi) {
            int dl = (int)(semi - vars->value);
            if (dl >= OPT_MAX_DESC_LEN) dl = OPT_MAX_DESC_LEN - 1;
            strncpy(o->desc, vars->value, dl);
            o->desc[dl] = '\0';
            while (dl > 0 && o->desc[dl-1] == ' ') o->desc[--dl] = '\0';

            const char *p = semi + 1;
            while (*p == ' ') p++;
            o->num_values = 0;

            while (*p && o->num_values < OPT_MAX_VALUES) {
                const char *pipe = strchr(p, '|');
                int len = pipe ? (int)(pipe - p) : (int)strlen(p);
                while (len > 0 && p[len-1] == ' ') len--;
                const char *start = p;
                while (len > 0 && *start == ' ') { start++; len--; }
                if (len > 0) {
                    if (len >= OPT_MAX_VAL_LEN) len = OPT_MAX_VAL_LEN - 1;
                    strncpy(o->values[o->num_values], start, len);
                    o->values[o->num_values][len] = '\0';
                    /* v0 has no labels, copy value as label */
                    strncpy(o->labels[o->num_values], o->values[o->num_values], OPT_MAX_LABEL_LEN - 1);
                    o->num_values++;
                }
                if (pipe) p = pipe + 1; else break;
            }
            o->selected = 0;
        } else {
            strncpy(o->desc, vars->value, OPT_MAX_DESC_LEN - 1);
        }
        num_options++;
    }
    apply_pending_overrides();
    printf("[options] Parsed %d core variables (v0)\n", num_options);
}

/* ── v1: parse retro_core_option_definition array ── */

static void parse_defs(const struct retro_core_option_definition *defs)
{
    if (!defs) return;
    num_options = 0;

    for (; defs->key != NULL && num_options < OPT_MAX_OPTIONS; defs++) {
        core_option_t *o = &options[num_options];
        memset(o, 0, sizeof(*o));
        strncpy(o->key, defs->key, OPT_MAX_KEY_LEN - 1);
        if (defs->desc)
            strncpy(o->desc, defs->desc, OPT_MAX_DESC_LEN - 1);
        o->visible = true;
        o->num_values = 0;

        for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && defs->values[i].value; i++) {
            if (o->num_values >= OPT_MAX_VALUES) break;
            strncpy(o->values[o->num_values], defs->values[i].value, OPT_MAX_VAL_LEN - 1);
            /* Use label if available, otherwise the raw value */
            const char *lbl = defs->values[i].label ? defs->values[i].label : defs->values[i].value;
            strncpy(o->labels[o->num_values], lbl, OPT_MAX_LABEL_LEN - 1);
            o->num_values++;
        }

        /* Find default */
        o->selected = 0;
        if (defs->default_value) {
            for (int i = 0; i < o->num_values; i++) {
                if (strcmp(o->values[i], defs->default_value) == 0) {
                    o->selected = i;
                    break;
                }
            }
        }
        num_options++;
    }
}

static const char *get_v2_desc(const struct retro_core_option_v2_definition *def)
{
    if (def->category_key && def->category_key[0] &&
        def->desc_categorized && def->desc_categorized[0])
        return def->desc_categorized;

    return def->desc;
}

static void parse_defs_v2(const struct retro_core_option_v2_definition *defs)
{
    if (!defs) return;
    num_options = 0;

    for (; defs->key != NULL && num_options < OPT_MAX_OPTIONS; defs++) {
        core_option_t *o = &options[num_options];
        const char *desc = get_v2_desc(defs);

        memset(o, 0, sizeof(*o));
        strncpy(o->key, defs->key, OPT_MAX_KEY_LEN - 1);
        if (desc)
            strncpy(o->desc, desc, OPT_MAX_DESC_LEN - 1);
        o->visible = true;
        o->num_values = 0;

        for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && defs->values[i].value; i++) {
            if (o->num_values >= OPT_MAX_VALUES) break;
            strncpy(o->values[o->num_values], defs->values[i].value, OPT_MAX_VAL_LEN - 1);
            {
                const char *lbl = defs->values[i].label ? defs->values[i].label : defs->values[i].value;
                strncpy(o->labels[o->num_values], lbl, OPT_MAX_LABEL_LEN - 1);
            }
            o->num_values++;
        }

        o->selected = 0;
        if (defs->default_value) {
            for (int i = 0; i < o->num_values; i++) {
                if (strcmp(o->values[i], defs->default_value) == 0) {
                    o->selected = i;
                    break;
                }
            }
        }

        num_options++;
    }
}

void opt_parse_v1(const void *data)
{
    const struct retro_core_option_definition *defs =
        (const struct retro_core_option_definition *)data;
    parse_defs(defs);
    apply_pending_overrides();
    printf("[options] Parsed %d core options (v1)\n", num_options);
}

/* ── v1 intl: use localized descriptions if available ── */

void opt_parse_v1_intl(const void *data)
{
    const struct retro_core_options_intl *intl =
        (const struct retro_core_options_intl *)data;

    if (!intl || !intl->us)
        return;

    /* Parse US English as base */
    parse_defs(intl->us);
    printf("[options] Parsed %d core options (v1 intl, us)\n", num_options);

    /* Overlay localized descriptions and labels if available */
    if (intl->local) {
        const struct retro_core_option_definition *loc = intl->local;
        for (; loc->key != NULL; loc++) {
            /* Find matching option */
            for (int i = 0; i < num_options; i++) {
                if (strcmp(options[i].key, loc->key) == 0) {
                    /* Override description */
                    if (loc->desc && loc->desc[0])
                        strncpy(options[i].desc, loc->desc, OPT_MAX_DESC_LEN - 1);

                    /* Override value labels (keep raw values from US) */
                    for (int v = 0; v < options[i].num_values; v++) {
                        /* Match by raw value */
                        for (int lv = 0; lv < RETRO_NUM_CORE_OPTION_VALUES_MAX && loc->values[lv].value; lv++) {
                            if (strcmp(options[i].values[v], loc->values[lv].value) == 0) {
                                if (loc->values[lv].label)
                                    strncpy(options[i].labels[v], loc->values[lv].label, OPT_MAX_LABEL_LEN - 1);
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }
        printf("[options] Applied localized overrides\n");
    }

    apply_pending_overrides();
}

void opt_parse_v2(const void *data)
{
    const struct retro_core_options_v2 *opts =
        (const struct retro_core_options_v2 *)data;

    if (!opts) return;

    parse_defs_v2(opts->definitions);
    apply_pending_overrides();
    printf("[options] Parsed %d core options (v2)\n", num_options);
}

void opt_parse_v2_intl(const void *data)
{
    const struct retro_core_options_v2_intl *intl =
        (const struct retro_core_options_v2_intl *)data;

    if (!intl || !intl->us) return;

    parse_defs_v2(intl->us->definitions);
    printf("[options] Parsed %d core options (v2 intl, us)\n", num_options);

    if (intl->local && intl->local->definitions) {
        const struct retro_core_option_v2_definition *loc = intl->local->definitions;

        for (; loc->key != NULL; loc++) {
            int option_index = find_option_index(loc->key);
            const char *desc = get_v2_desc(loc);

            if (option_index < 0)
                continue;

            if (desc && desc[0])
                strncpy(options[option_index].desc, desc, OPT_MAX_DESC_LEN - 1);

            for (int v = 0; v < options[option_index].num_values; v++) {
                for (int lv = 0; lv < RETRO_NUM_CORE_OPTION_VALUES_MAX && loc->values[lv].value; lv++) {
                    if (strcmp(options[option_index].values[v], loc->values[lv].value) == 0) {
                        if (loc->values[lv].label)
                            strncpy(options[option_index].labels[v], loc->values[lv].label, OPT_MAX_LABEL_LEN - 1);
                        break;
                    }
                }
            }
        }
        printf("[options] Applied localized overrides (v2)\n");
    }

    apply_pending_overrides();
}

/* ── GET_VARIABLE ── */

bool get_option(const char *key, const char **value)
{
    for (int i = 0; i < num_options; i++) {
        if (strcmp(options[i].key, key) == 0) {
            if (options[i].num_values > 0 && options[i].selected < options[i].num_values) {
                *value = options[i].values[options[i].selected];
                return true;
            }
            break;
        }
    }

    {
        const char *pending_value = find_pending_override_value(key);
        if (pending_value && pending_value[0]) {
            *value = pending_value;
            return true;
        }
    }

    return false;
}

/* ── ini handler ── */

int opt_handler(void *user, const char *section, const char *name, const char *value)
{
    (void)user; (void)section;
    remember_override(name, value);
    apply_override(name, value);
    return 1;
}

int opt_load(const char *path)
{
    return ini_parse(path, opt_handler, NULL);
}

int opt_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# Core options (auto-saved)\n");
    for (int i = 0; i < num_options; i++) {
        if (options[i].num_values > 0) {
            fprintf(f, "%s = ", options[i].key);
            write_quoted_option_value(f, options[i].values[options[i].selected]);
            fputc('\n', f);
        }
    }
    for (int i = 0; i < num_pending_overrides; i++) {
        if (find_option_index(pending_overrides[i].key) >= 0)
            continue;

        fprintf(f, "%s = ", pending_overrides[i].key);
        write_quoted_option_value(f, pending_overrides[i].value);
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

bool opt_override(const char *key, const char *value)
{
    if (!key || !value || !key[0] || !value[0])
        return false;

    remember_override(key, value);

    if (find_option_index(key) < 0)
        return false;

    apply_override(key, value);
    vars_updated = true;
    return true;
}

bool opt_has_value(const char *key, const char *value)
{
    int option_index = 0;
    char normalized[OPT_MAX_VAL_LEN];

    normalize_option_value(value, normalized, sizeof(normalized));

    if (!key || !value || !key[0] || !value[0])
        return false;

    option_index = find_option_index(key);
    if (option_index < 0)
        return false;

    for (int i = 0; i < options[option_index].num_values; i++) {
        if (strcmp(options[option_index].values[i], normalized) == 0)
            return true;
    }

    return false;
}

bool opt_override_first_available(const char *key, const char *const *values, int count, const char **selected_value)
{
    if (selected_value)
        *selected_value = NULL;

    if (!key || !values || count <= 0)
        return false;

    for (int i = 0; i < count; i++) {
        const char *candidate = values[i];

        if (!candidate || !candidate[0])
            continue;

        if (!opt_has_value(key, candidate))
            continue;

        if (!opt_override(key, candidate))
            return false;

        if (selected_value)
            *selected_value = candidate;
        return true;
    }

    return false;
}

bool opt_value_allowed(const char *key, const char *value)
{
    return is_option_value_allowed(key, value);
}

bool opt_updated(void)       { return vars_updated; }
void opt_clear_updated(void) { vars_updated = false; }
bool opt_set_visible(const char *key, bool visible)
{
    int option_index = find_option_index(key);

    if (option_index < 0)
        return false;

    options[option_index].visible = visible;
    return true;
}

int opt_count(void) { return num_options; }

const core_option_t *opt_get_entry(int index)
{
    if (index >= 0 && index < num_options) return &options[index];
    return NULL;
}

void opt_cycle(int index, int dir)
{
    if (index < 0 || index >= num_options) return;
    core_option_t *o = &options[index];
    if (o->num_values <= 0) return;
    int start = o->selected;
    int next = start;

    do {
        next = (next + dir + o->num_values) % o->num_values;
        if (is_option_value_allowed(o->key, o->values[next])) {
            o->selected = next;
            vars_updated = true;
            return;
        }
    } while (next != start);
}
