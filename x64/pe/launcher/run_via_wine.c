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
 *                        UWPH_WOW64_DLL_ROOT selects a paired Wine DLL root
 *                        (i386-windows PEs + x86_64-unix companions).
 *
 * W^X bundle (uwp-host harness build, -DRUN_VIA_WINE_WX_LOADER="..."): when
 * the uwph shim is loaded it refuses every PROT_WRITE|PROT_EXEC request, and
 * the stock loader's ntdll.so maps PE images RWX, so the stock loader cannot
 * run there. In that case the stock loader is swapped for the bundled W^X
 * loader (R4X wine-image-wx.patch: map RW, relocate, then per-section RX),
 * and the swap is printed. Any other WINE_LOADER is used exactly as given;
 * RUN_VIA_WINE_EXACT_LOADER=1 disables the swap (negative controls).
 */
#ifdef RUN_VIA_WINE_WX_LOADER
#define _GNU_SOURCE
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

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

    /* New-style WoW64 needs i386 Windows PEs paired with x86-64 Unix
     * companions in ONE Wine DLL root. A distro i386-only root cannot
     * initialize ws2_32 (or crypt32). Opt in with a packaged paired root;
     * never silently fall back to the broken layout. */
    const char *wow64_root = getenv("UWPH_WOW64_DLL_ROOT");
    if (wow64_root) {
        const char *parts[] = {"i386-windows", "x86_64-unix"};
        for (unsigned i = 0; i < 2; ++i) {
            size_t n = strlen(wow64_root) + strlen(parts[i]) + 2;
            char *path = malloc(n);
            struct stat st;
            if (!path) return 127;
            snprintf(path, n, "%s/%s", wow64_root, parts[i]);
            if (!*wow64_root || stat(path, &st) || !S_ISDIR(st.st_mode)) {
                fprintf(stderr, "[launcher] incomplete WoW64 DLL root: %s\n", path);
                free(path);
                return 127;
            }
            free(path);
        }
        if (setenv("WINEDLLPATH", wow64_root, 1)) return 127;
        fprintf(stderr, "[launcher] paired WoW64 DLL root: %s\n", wow64_root);
    }

#ifdef RUN_VIA_WINE_WX_LOADER
    /* uwph_shim_report is exported only by the uwph shim (shim.map), whose
     * mmap wrapper refuses WRITE|EXEC: W^X is enforced in this process tree. */
    const char *exact = getenv("RUN_VIA_WINE_EXACT_LOADER");
    if (dlsym(RTLD_DEFAULT, "uwph_shim_report") && !(exact && *exact == '1') &&
        !strcmp(loader, "/usr/lib/wine/wine64")) {
        if (access(RUN_VIA_WINE_WX_LOADER, X_OK)) {
            fprintf(stderr, "[launcher] W^X enforced but bundled W^X loader %s is missing\n",
                    RUN_VIA_WINE_WX_LOADER);
            return 127;
        }
        fprintf(stderr, "[launcher] W^X enforced by uwph shim: stock %s maps PE images RWX; "
                "using bundled W^X loader %s (RW map/relocate, then RX)\n",
                loader, RUN_VIA_WINE_WX_LOADER);
        loader = RUN_VIA_WINE_WX_LOADER;
    }
#endif

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
