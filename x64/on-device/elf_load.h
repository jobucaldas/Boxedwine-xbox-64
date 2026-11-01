#ifndef ELF_LOAD_H
#define ELF_LOAD_H
#include <wchar.h>
/* Maps only; no constructors, guest process startup or TLS execution in W1. */
int elf_load_test(const wchar_t *directory, const wchar_t *state, void (*log)(const char *));
#endif
