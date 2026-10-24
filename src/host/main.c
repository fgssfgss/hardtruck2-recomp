/*
 * main.c - the Hard Truck 2 host: the lifted king.exe on pcrecomp's win32hle.
 *
 *   ht2 [--game DIR] [-- game args...]
 *
 * DIR is the install (default: $HT2_GAME, else ./game). It becomes C:\HardTruck2
 * and the current directory, which is what the GOG install looks like to the
 * game. The boot is permissive: an import nothing answers yet binds to a stub
 * that stops the run naming itself (win32hle README, "What a title still
 * brings") -- implement it in src/shims/, rerun, get the next one.
 */
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "win32hle.h"
#include "host.h"
#include "ht2_shims.h"

int main(int argc, char **argv) {
    const char *game = getenv("HT2_GAME");
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(stderr, "usage: %s [--game DIR] [-- game args...]\n", argv[0]); return 64; }
    }
    char root[PATH_MAX];
    if (!realpath(game ? game : "game", root)) {
        fprintf(stderr, "[ht2] game directory %s not found (--game or $HT2_GAME)\n",
                game ? game : "game");
        return 66;
    }
    if (chdir(root) != 0) { perror(root); return 66; }

    char exe[PATH_MAX + 16];
    snprintf(exe, sizeof exe, "%s/king.exe", root);

    /* C:\ is the parent of the install, so the install is C:\HardTruck2. */
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", root);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0;
    hle_set_drive('C', parent);

    char cmdline[1024];
    int n = snprintf(cmdline, sizeof cmdline, "\"C:\\HardTruck2\\king.exe\"");
    for (; i < argc && n < (int)sizeof cmdline; i++)
        n += snprintf(cmdline + n, sizeof cmdline - n, " %s", argv[i]);
    hle_set_command_line(cmdline);
    hle_set_module_path(exe);

    win32hle_trace = getenv("HT2_TRACE") != NULL;   /* one line per import / COM method */
    ht2_shims_register();          /* first: name lookup is first-match, ours win */

    recomp_host_init();
    if (recomp_host_boot_permissive(exe) != 0) return 2;
    return (int)recomp_host_run();
}
