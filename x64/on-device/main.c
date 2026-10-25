/* Desktop build smoke only; the UWP activation bridge is not implemented. */
#include <stdio.h>

#if !defined(_WIN64) || !defined(__x86_64__)
#error This skeleton requires an x64 Windows target.
#endif

int main(void)
{
    puts("Boxedwine on-device skeleton: x64 Windows build OK");
    puts("Manifest: codeGeneration, internetClient, privateNetworkClientServer");
    puts("Game/Title memory: configure Game in Developer Home; budget unverified.");
    puts("Status: build-only; UWP activation and Wine loader integration pending.");
    return 0;
}
