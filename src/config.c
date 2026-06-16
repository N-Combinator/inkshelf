/*
 * config.c — flat key=value settings store (see config.h).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "config.h"

#ifndef INKSHELF_CONF_PATH
#define INKSHELF_CONF_PATH "/mnt/ext1/system/config/inkshelf.conf"
#endif

#define CONF_PATH_MAX 512
#define CONF_LINE_MAX (CONFIG_VALUE_MAX + 64)

static char g_path[CONF_PATH_MAX] = INKSHELF_CONF_PATH;

void config_set_path(const char *path)
{
    if (path && path[0])
        snprintf(g_path, sizeof g_path, "%s", path);
    else
        snprintf(g_path, sizeof g_path, "%s", INKSHELF_CONF_PATH);
}

/* Trim a trailing CR/LF in place. */
static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

int config_get(const char *key, char *out, size_t outsz)
{
    if (out && outsz) out[0] = '\0';
    if (!key || !key[0] || !out || !outsz) return -1;

    FILE *f = fopen(g_path, "r");
    if (!f) return -1;

    size_t klen = strlen(key);
    char line[CONF_LINE_MAX];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            snprintf(out, outsz, "%s", line + klen + 1);
            found = 0;
            /* keep scanning so the LAST occurrence wins (matches how
             * config_set appends), but a single value is the norm */
        }
    }
    fclose(f);
    return found;
}

/* mkdir -p of the directory holding g_path (best-effort). */
static void ensure_parent_dir(void)
{
    char dir[CONF_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", g_path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(dir, 0755); *p = '/'; }
    }
    mkdir(dir, 0755);
}

int config_set(const char *key, const char *value)
{
    if (!key || !key[0]) return -1;
    if (!value) value = "";

    ensure_parent_dir();

    /* Read existing lines, dropping any prior assignment of this key. */
    char tmp[CONF_PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", g_path) >= sizeof tmp)
        return -1;

    FILE *out = fopen(tmp, "w");
    if (!out) return -1;

    size_t klen = strlen(key);
    FILE *in = fopen(g_path, "r");
    if (in) {
        char line[CONF_LINE_MAX];
        while (fgets(line, sizeof line, in)) {
            char trimmed[CONF_LINE_MAX];
            snprintf(trimmed, sizeof trimmed, "%s", line);
            chomp(trimmed);
            if (trimmed[0] != '#' &&
                strncmp(trimmed, key, klen) == 0 && trimmed[klen] == '=')
                continue;                       /* drop old value */
            fputs(line, out);
            size_t n = strlen(line);
            if (n == 0 || line[n - 1] != '\n') fputc('\n', out);
        }
        fclose(in);
    }
    fprintf(out, "%s=%s\n", key, value);

    if (fclose(out) != 0) { remove(tmp); return -1; }
    if (rename(tmp, g_path) != 0) { remove(tmp); return -1; }
    return 0;
}
