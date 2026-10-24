/*
 * run_via_wine.c — a tiny launcher that starts a guest x86-64 PE by handing it
 * to Wine's OWN loader binary (wine64). This is Wine's public invocation
 * mechanism: we exec the installed loader and pass the guest .exe path as an
 * argument. The guest image is input DATA to Wine's mature, open-source PE
 * loader; this launcher does NO image mapping, NO relocation, NO import
 * resolution itself. It is the exact piece that would live inside the Dev Mode
 * UWP app, calling into the Wine loader bundled in the app's own storage.
 *
 * Build (native Linux):  cc -O2 -o run_via_wine run_via_wine.c
 * Usage:                 ./run_via_wine <guest.exe> [args...]
 * Env:                   WINE_LOADER may override the loader path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <guest.exe> [args...]\n", argv[0]);
        return 2;
    }

    /* Wine's public loader binary. Inside the UWP app this path points at the
     * Wine loader shipped in the app's install dir; here it is the distro one. */
    const char *loader = getenv("WINE_LOADER");
    if (!loader) loader = "/usr/lib/wine/wine64";

    /* argv for the loader: loader, guest.exe, [guest args...], NULL */
    char **av = calloc(argc + 1, sizeof(*av));
    av[0] = (char *)loader;
    for (int i = 1; i < argc; i++) av[i] = argv[i];
    av[argc] = NULL;

    fprintf(stderr, "[launcher] exec %s -> %s (handing guest to Wine's loader)\n",
            loader, argv[1]);
    fflush(stderr);

    execv(loader, av);           /* replace ourselves with Wine's loader */
    perror("[launcher] execv");  /* only reached on failure */
    return 127;
}
