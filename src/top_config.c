/*
 * top_config.c — reading ~/.config/GnuChanTop/GnuChanTop.conf.
 *
 * The file is plain Key=value text and is never executed. The `#` starts a
 * comment and blank lines are skipped. A line this does not understand is
 * skipped rather than being a reason to stop: a monitor that will not start
 * because of one bad setting is worse than one that starts on the defaults.
 *
 * HOME is what the path is built from; when it is unset the file is simply not
 * found and the defaults stand, which is what a program run from a bare init
 * wants.
 */
#include "top_config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the defaults ---------------------------------------------------------- */

void top_config_defaults(TopConfig *config) {
    config->update_time = 1.0;
    config->proc_limit = 200;
    config->show_gpu = 1;
    config->sort = TOP_SORT_CPU;
    config->error[0] = '\0';
}

/* --- sort key names -------------------------------------------------------- */

const char *top_sort_name(TopSortKey key) {
    switch (key) {
        case TOP_SORT_MEM:  return "mem";
        case TOP_SORT_PID:  return "pid";
        case TOP_SORT_NAME: return "name";
        case TOP_SORT_CPU:
        default:            return "cpu";
    }
}

int top_sort_from_name(const char *name, TopSortKey *out) {
    if (strcmp(name, "cpu") == 0)  { *out = TOP_SORT_CPU;  return 0; }
    if (strcmp(name, "mem") == 0)  { *out = TOP_SORT_MEM;  return 0; }
    if (strcmp(name, "pid") == 0)  { *out = TOP_SORT_PID;  return 0; }
    if (strcmp(name, "name") == 0) { *out = TOP_SORT_NAME; return 0; }
    return -1;
}

/* --- text helpers ---------------------------------------------------------- */

/* Trim the leading and trailing spaces off a string, in place. The return is
   the first non-space character, which is what the caller wants to keep. */
static char *trim(char *text) {
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static char *unquote(char *text) {
    size_t length = strlen(text);
    if (length >= 2 &&
        ((text[0] == '"' && text[length - 1] == '"') ||
         (text[0] == '\'' && text[length - 1] == '\''))) {
        text[length - 1] = '\0';
        return text + 1;
    }
    return text;
}

/* Parse an integer or decimal without accepting partial values such as
   `12junk`. Invalid values leave their setting at the built-in default. */
static int parse_double(const char *text, double *out) {
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || errno == ERANGE || !isfinite(value) ||
        *trim(end) != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

static int parse_int(const char *text, int *out) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (end == text || errno == ERANGE || *trim(end) != '\0' ||
        value < 1 || value > 2147483647L) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int parse_bool(const char *value, int *out) {
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
        strcmp(value, "True") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "Yes") == 0 || strcmp(value, "on") == 0 ||
        strcmp(value, "On") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
        strcmp(value, "False") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "No") == 0 || strcmp(value, "off") == 0 ||
        strcmp(value, "Off") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

/* --- the file -------------------------------------------------------------- */

/* The path the settings are read from, or an empty string when HOME is not set.
   XDG_CONFIG_HOME is honoured first, which is what a session that has moved its
   config directory expects. */
static void config_path(char *out, size_t size) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, size, "%s/GnuChanTop/GnuChanTop.conf", xdg);
        return;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, size, "%s/.config/GnuChanTop/GnuChanTop.conf", home);
        return;
    }
    out[0] = '\0';
}

int top_config_load(TopConfig *config) {
    top_config_defaults(config);

    char path[TOP_TEXT * 2];
    config_path(path, sizeof(path));
    if (!path[0]) {
        snprintf(config->error, sizeof(config->error),
                 "HOME is not set, so no settings file was looked for");
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (!file && errno == ENOENT) {
        size_t length = strlen(path);
        if (length >= 5 && strcmp(path + length - 5, ".conf") == 0 &&
            length - 5 + 4 <= sizeof(path)) {
            char legacy_path[sizeof(path)];
            memcpy(legacy_path, path, length - 5);
            memcpy(legacy_path + length - 5, ".py", 4);
            file = fopen(legacy_path, "r");
        }
    }
    if (!file) {
        /* No file is normal, and not worth a message: the defaults are the
           settings a machine that never wrote one wants. */
        snprintf(config->error, sizeof(config->error),
                 "no settings file was found");
        return -1;
    }

    char line[TOP_TEXT];
    while (fgets(line, sizeof(line), file)) {
        /* Cut the comment off first: a `#` anywhere on the line starts it, and
           nothing after it is a value. Then trim what is left. */
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *text = trim(line);
        if (!text[0]) {
            continue;
        }

        /* Split at the first `=`. A line without one is not a setting. */
        char *equals = strchr(text, '=');
        if (!equals) {
            continue;
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = unquote(trim(equals + 1));

        if (strcmp(key, "UpdateTime") == 0) {
            double seconds = 0.0;
            if (parse_double(value, &seconds) == 0 && seconds > 0.05) {
                config->update_time = seconds;
            }
        } else if (strcmp(key, "ProcLimit") == 0) {
            int limit = 0;
            if (parse_int(value, &limit) == 0) {
                config->proc_limit = limit;
            }
        } else if (strcmp(key, "ShowGPU") == 0) {
            int show_gpu = 0;
            if (parse_bool(value, &show_gpu) == 0) {
                config->show_gpu = show_gpu;
            }
        } else if (strcmp(key, "Sort") == 0) {
            TopSortKey key_value;
            if (top_sort_from_name(value, &key_value) == 0) {
                config->sort = key_value;
            }
        }
        /* Any other key is a setting this version does not know, which is not
           an error: a newer file than the program is expected to keep working. */
    }

    fclose(file);
    return 0;
}
