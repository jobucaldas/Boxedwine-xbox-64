#include "slots.h"
int main(void) {
    const char *names[] = {"InvalidateCredentials", "DestroyCachedCredentials", "LogOffAll",
        "LogOff", "RevokeRefreshToken", "ForgetCredential", "clear-login-users"};
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
        struct host_slot entry = {"IClientUser", names[i], 999, 0, NULL};
        /* NULL vtable proves resolution cannot read any real denied pointer. */
        if (resolve_slot(&entry, NULL) != (void *)denied_trap) return 1;
    }
    struct host_slot evidence_denied = {"IClientUser", "Unknown", 999, 1, (void *)denied_trap};
    if (resolve_slot(&evidence_denied, NULL) != (void *)denied_trap) return 2;
    void *table[] = {(void *)main};
    void **object = table;
    struct host_slot allowed = {"IClientEngine", "RunFrame", 0, 0, NULL};
    if (resolve_slot(&allowed, &object) != (void *)main) return 3;
    if (getenv("TEST_TRAP")) {
        resolve_slot(&evidence_denied, NULL);
        denied_trap();
    }
    return 0;
}
