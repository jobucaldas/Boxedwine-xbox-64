/* Minimal 64-bit Windows program: kernel32 only, no CRT.
 * x86_64-w64-mingw32-gcc -nostdlib -e entry -o hello_k32.exe hello_k32.c -lkernel32 */
#include <windows.h>

void entry(void) {
    static const char msg[] = "hello from a native x86-64 PE (kernel32 only)\r\n";
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), msg, sizeof(msg) - 1, &written, NULL);
    ExitProcess(0);
}
