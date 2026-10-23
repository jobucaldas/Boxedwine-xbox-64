/* Static glibc x86-64 test: printf, malloc, 64-bit arithmetic, string ops. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint64_t fib(int n) { return n < 2 ? (uint64_t)n : fib(n - 1) + fib(n - 2); }

int main(int argc, char **argv) {
    printf("hello from x86-64 glibc guest, argc=%d argv[0]=%s\n", argc, argv[0]);
    uint64_t big = 0x123456789ABCDEFULL;
    big = big * 31 + (big >> 7);
    printf("64-bit math: %llx fib(27)=%llu\n", (unsigned long long)big, (unsigned long long)fib(27));
    char *buf = malloc(1 << 20);
    for (int i = 0; i < (1 << 20); i++) buf[i] = (char)('a' + i % 26);
    buf[100] = 0;
    printf("malloc ok, strlen=%zu sizeof(void*)=%zu\n", strlen(buf), sizeof(void *));
    double d = 1.0;
    for (int i = 1; i <= 10; i++) d *= i * 0.5;
    printf("double: %.3f\n", d);
    free(buf);
    return 0;
}
