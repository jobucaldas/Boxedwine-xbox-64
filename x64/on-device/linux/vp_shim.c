/* B61: the mingw CRT's pseudo-relocation code imports VirtualProtect from KERNEL32,
 * which is outside the app partition. Route it to the documented app API
 * VirtualProtectFromApp (codeGeneration capability rules still apply unchanged).
 * Both the direct symbol and its __imp_ slot are defined so libkernel32.a's member
 * (which carries the KERNEL32 import) is never pulled in. */
#define VirtualProtect VirtualProtect_kernel32_unused
#include <windows.h>
#undef VirtualProtect
WINBASEAPI BOOL WINAPI VirtualProtectFromApp(PVOID, SIZE_T, ULONG, PULONG);
BOOL WINAPI b61_virtual_protect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old) __asm__("VirtualProtect");
BOOL WINAPI b61_virtual_protect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old)
{
    ULONG o = 0;
    BOOL ok = VirtualProtectFromApp(addr, size, prot, &o);
    if (old) *old = o;
    return ok;
}
void *b61_imp_virtual_protect __asm__("__imp_VirtualProtect") = (void *)b61_virtual_protect;
