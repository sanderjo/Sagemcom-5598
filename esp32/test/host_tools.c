// Host build of the tool layer (tools.c + mesh.c + pyjson.c): runs one tool
// against saved router replies and prints its result (or error) as JSON.
// Driven by compare_tools.py; fixture names as in capture_fixtures.py.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tools.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) exit(1);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

// "/api/v2/firewall/chain?chain=Custom" -> "<dir>/api_v2_firewall_chain_chain-Custom.json"
static cJSON *fetch(void *ctx, const char *path, char *err, size_t err_size)
{
    char file[512];
    size_t o = snprintf(file, sizeof(file), "%s/", (const char *)ctx);
    for (const char *p = path + 1; *p && o + 6 < sizeof(file); p++)
        file[o++] = *p == '/' || *p == '?' ? '_' : *p == '=' ? '-' : *p;
    snprintf(file + o, sizeof(file) - o, ".json");
    char *text = slurp(file);
    if (!text) {
        snprintf(err, err_size, "no fixture for %s", path);
        return NULL;
    }
    cJSON *json = cJSON_Parse(text);
    free(text);
    if (!json) snprintf(err, err_size, "invalid JSON from %s", path);
    return json;
}

static size_t load_nicknames(char *text, nickname_t *out, size_t max)
{
    size_t n = 0;
    char *line = text, *next;
    for (; line && *line && n < max; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0'; else next = line + strlen(line);
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        while (isspace((unsigned char)*line)) line++;
        char *sep = line + strcspn(line, " \t");
        if (!*line || !*sep) continue;
        *sep++ = '\0';
        while (isspace((unsigned char)*sep)) sep++;
        char *end = sep + strlen(sep);
        while (end > sep && isspace((unsigned char)end[-1])) *--end = '\0';
        if (!*sep) continue;
        for (char *p = line; *p; p++) *p = tolower((unsigned char)*p);
        out[n++] = (nickname_t){line, sep};
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s FIXTURE_DIR TOOL ARGS_JSON [nicknames.txt]\n", argv[0]); return 2; }
    char *nick_text = argc > 4 ? slurp(argv[4]) : NULL;
    nickname_t nicknames[256];
    size_t count = load_nicknames(nick_text, nicknames, 256);
    cJSON *args = cJSON_Parse(argv[3]);
    char err[200];
    cJSON *out = tools_run(argv[2], args, fetch, argv[1], nicknames, count, err, sizeof(err));
    if (!out) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "__error__", err);
        out = e;
    }
    char *s = cJSON_PrintUnformatted(out);
    puts(s);
    free(s);
    cJSON_Delete(out);
    cJSON_Delete(args);
    free(nick_text);
    return 0;
}
