/* B44 O2: GnuTLS provider for the uwph Win32 backend (decision (a) in
 * docs/tls-on-xbox.md).  The x86_64-w64-mingw32 GnuTLS built by
 * build-gnutls.sh is linked in and registered as "libgnutls.so.30", so
 * Wine's bcrypt/crypt32/secur32 Unix halves find it through uwph_dlopen /
 * uwph_dlsym exactly as they would dlopen the host library on Linux. */
#include "uwph/uwph.h"

/* Only addresses are taken; prototypes come from the real headers in the
 * consumers (Wine), so declare each entry point opaquely. */
#define X(f) extern void f(void);
#include "wine_gnutls_symbols.h"
#undef X

static const struct uwph_dl_symbol gnutls_symbols[] = {
#define X(f) { #f, (void *)f },
#include "wine_gnutls_symbols.h"
#undef X
    { 0, 0 }
};

static const struct uwph_dl_library gnutls_library = {
    "libgnutls.so.30", gnutls_symbols, 0, 0
};

int uwph_tls_provider_register(void)
{
    return uwph_dl_register(&gnutls_library);
}
