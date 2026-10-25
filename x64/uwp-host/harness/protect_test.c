#include "uwph/uwph.h"
#include <stdio.h>
#include <string.h>
int main(void)
{
    /* Native x64 code, in this process only: mov eax,42; ret. */
    const unsigned char code[] = {0xb8,42,0,0,0,0xc3};
    void *p=uwph_mmap(NULL,4096,UWPH_PROT_READ|UWPH_PROT_WRITE,
                     UWPH_MAP_PRIVATE|UWPH_MAP_ANONYMOUS,-1,0);
    if (p==UWPH_MAP_FAILED) return 1;
    memcpy(p,code,sizeof(code));
    if (uwph_protect(p,4096,UWPH_PROT_READ|UWPH_PROT_EXEC)) return 2;
    if (((int (*)(void))p)()!=42) return 3;
    if (uwph_protect(p,4096,UWPH_PROT_WRITE|UWPH_PROT_EXEC)!=-1 || uwph_errno!=UWPH_EACCES) return 4;
    if (uwph_munmap(p,4096)) return 5;
    puts("uwph_protect RW->RX native execution=42; WRITE|EXEC rejected: PASS (host API only, no PE)");
    return 0;
}
