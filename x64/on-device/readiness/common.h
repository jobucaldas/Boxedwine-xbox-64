#include <windows.h>
#include <stdio.h>
static int g_pass, g_fail;
#define CHECK(name, cond, ...) do { int _c = !!(cond); if (_c) g_pass++; else g_fail++; \
    printf("%s %-42s ", _c ? "PASS" : "FAIL", name); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define INFO(...) do { printf("INFO "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
#define SUMMARY(probe) do { printf("SUMMARY %s pass=%d fail=%d\n", probe, g_pass, g_fail); fflush(stdout); return g_fail ? 1 : 0; } while (0)
