// Host build of ../src/mesh.c: prints extenders, devices and topology for a
// saved meshdevices reply as one JSON object. Nicknames file optional.
// Built and compared against the Python client by compare_mesh.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "mesh.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) exit(1);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

// same format as nicknames.txt: "<name> <nickname>", '#' comments
// (parses `text` in place; the entries point into it)
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
    if (argc < 2) { fprintf(stderr, "usage: %s meshdevices.json [nicknames.txt]\n", argv[0]); return 2; }
    char *text = slurp(argv[1]);
    cJSON *reply = cJSON_Parse(text);
    free(text);
    nickname_t nicknames[256];
    char *nick_text = argc > 2 ? slurp(argv[2]) : NULL;
    size_t count = load_nicknames(nick_text, nicknames, 256);

    cJSON *out = cJSON_CreateObject();
    cJSON *ext = mesh_extenders(reply), *dev = mesh_devices(reply), *topo = mesh_topology(reply);
    cJSON_AddItemToObject(out, "extenders", ext ? ext : cJSON_CreateNull());
    cJSON_AddItemToObject(out, "devices", dev ? dev : cJSON_CreateNull());
    cJSON_AddItemToObject(out, "topology", topo ? topo : cJSON_CreateNull());
    mesh_add_nicknames(out, nicknames, count);
    char *s = cJSON_PrintUnformatted(out);
    puts(s);
    free(s);
    cJSON_Delete(out);
    cJSON_Delete(reply);
    free(nick_text);
    return 0;
}
