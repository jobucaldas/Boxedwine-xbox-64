/* Our call table contains a trap for every denied entry; never its real pointer. */
#ifndef HOST_SLOTS_H
#define HOST_SLOTS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
struct host_slot { const char *iface_name, *name; unsigned index; int denied; void *target; };
static int denied_name(const char *name) {
    char normalized[512];
    size_t n = 0;
    for (; *name && n + 1 < sizeof(normalized); ++name)
        if (isalnum((unsigned char)*name)) normalized[n++] = (char)tolower((unsigned char)*name);
    normalized[n] = 0;
    return strstr(normalized, "credential") || strstr(normalized, "revoke") ||
        strstr(normalized, "invalidate") || strstr(normalized, "logoff") ||
        strstr(normalized, "clearloginusers");
}
static const char *denied_slot_name;
static void denied_trap(void) {
    fprintf(stderr, "denied_slot=%s\n", denied_slot_name ? denied_slot_name : "unknown");
    fflush(stderr);
    abort();
}
static void *resolve_slot(const struct host_slot *entry, void *object) {
    if (entry->denied || denied_name(entry->name)) {
        denied_slot_name = entry->name;
        return (void *)denied_trap;
    }
    if (entry->target) return entry->target;
    return (*(void ***)object)[entry->index];
}
#endif
