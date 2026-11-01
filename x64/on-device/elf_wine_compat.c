/* Off-device ONLY: stock Wine lacks VirtualProtectFromApp. Never linked into
 * rungs.exe. Keep the exact loader object; adapt only its import slot. */
#include <windows.h>
__declspec(dllimport) BOOL WINAPI VirtualProtect(LPVOID,SIZE_T,DWORD,PDWORD);
static BOOL WINAPI wine_protect(PVOID p,SIZE_T n,ULONG prot,PULONG old)
{return VirtualProtect(p,n,prot,old);}
void *elf_wine_protect_import __asm__("__imp_VirtualProtectFromApp") = (void *)wine_protect;
