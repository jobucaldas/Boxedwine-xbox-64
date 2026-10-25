/* THROUGH-uwph Linux prototype; no Wine or console execution claim. */
#include "uwph/uwph.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#define CHUNK (1LL<<30)
static void size_is(int fd, long long size)
{
    struct uwph_stat st;
    assert(uwph_fstat(fd,&st)==0 && st.st_size==size);
}
int main(void)
{
    char dir[]="/tmp/uwph-bigfile-XXXXXX", path[256], part[300];
    assert(mkdtemp(dir)); snprintf(path,sizeof(path),"%s/sparse",dir);
    unsetenv("UWPH_BIGFILE_SPLIT");
    int fd=uwph_open(path,UWPH_O_CREAT|UWPH_O_EXCL|UWPH_O_RDWR,0600);
    assert(fd>=0); assert(access(path,F_OK)==0);
    /* Preserve pre-existing default ftruncate ENOSYS behavior. */
    assert(uwph_ftruncate(fd,3*CHUNK)==-1 && uwph_errno==UWPH_ENOSYS);
    assert(uwph_close(fd)==0); assert(unlink(path)==0);
    setenv("UWPH_BIGFILE_SPLIT","0",1);
    fd=uwph_open(path,UWPH_O_CREAT|UWPH_O_RDWR,0600);
    assert(fd>=0 && access(path,F_OK)==0); assert(uwph_close(fd)==0); assert(unlink(path)==0);
    setenv("UWPH_BIGFILE_SPLIT","1",1);
    fd=uwph_open(path,UWPH_O_CREAT|UWPH_O_EXCL|UWPH_O_RDWR,0600);
    assert(fd>=0 && access(path,F_OK)==-1);
    assert(uwph_ftruncate(fd,3*CHUNK)==0); size_is(fd,3*CHUNK);
    long long blocks=0;
    for(int i=0;i<4;i++) {
        struct stat st; snprintf(part,sizeof(part),"%s.uwphpart%d",path,i);
        assert(stat(part,&st)==0 && st.st_size==(i==3 ? 0 : CHUNK)); blocks+=st.st_blocks;
    }
    assert(blocks<1024); /* genuinely sparse, not a 3 GiB RAM/disk allocation */
    char data[8192], readback[8192];
    memset(data,0xa7,sizeof(data));
    assert(uwph_pwrite(fd,data,sizeof(data),2*CHUNK-4096)==sizeof(data));
    assert(uwph_pread(fd,readback,sizeof(readback),2*CHUNK-4096)==sizeof(data));
    assert(!memcmp(data,readback,sizeof(data)));
    assert(uwph_lseek(fd,-4096,UWPH_SEEK_END)==3*CHUNK-4096);
    assert(uwph_lseek(fd,2*CHUNK-4096,UWPH_SEEK_SET)==2*CHUNK-4096);
    assert(uwph_read(fd,readback,sizeof(data))==sizeof(data));
    assert(!memcmp(data,readback,sizeof(data)));
    assert(uwph_lseek(fd,0,UWPH_SEEK_CUR)==2*CHUNK+4096);
    memset(readback,1,sizeof(readback));
    assert(uwph_pread(fd,readback,sizeof(readback),CHUNK+4096)==sizeof(readback));
    for(unsigned i=0;i<sizeof(readback);i++) assert(!readback[i]);
    assert(uwph_pread(fd,readback,1,3*CHUNK)==0);
    assert(uwph_pread(fd,readback,1,-1)==-1 && uwph_errno==UWPH_EINVAL);
    assert(uwph_ftruncate(fd,-1)==-1 && uwph_errno==UWPH_EINVAL);
    assert(uwph_ftruncate(fd,1024*CHUNK)==-1 && uwph_errno==UWPH_EFBIG);
    void *map=uwph_mmap(NULL,sizeof(data),UWPH_PROT_READ,UWPH_MAP_PRIVATE,fd,2*CHUNK-4096);
    assert(map!=UWPH_MAP_FAILED && !memcmp(map,data,sizeof(data)));
    assert(uwph_munmap(map,sizeof(data))==0);
    assert(uwph_mmap(NULL,4096,UWPH_PROT_READ|UWPH_PROT_WRITE,UWPH_MAP_PRIVATE,fd,0)==UWPH_MAP_FAILED);
    assert(uwph_mmap(NULL,4096,UWPH_PROT_READ,UWPH_MAP_PRIVATE,fd,1)==UWPH_MAP_FAILED);
    assert(uwph_close(fd)==0);
    fd=uwph_open(path,UWPH_O_RDONLY); assert(fd>=0); size_is(fd,3*CHUNK);
    assert(uwph_pread(fd,readback,sizeof(data),2*CHUNK-4096)==sizeof(data));
    assert(!memcmp(data,readback,sizeof(data)));
    assert(uwph_pwrite(fd,data,1,0)==-1 && uwph_errno==UWPH_EBADF);
    assert(uwph_ftruncate(fd,0)==-1 && uwph_errno==UWPH_EBADF);
    assert(uwph_close(fd)==0);
    fd=uwph_open(path,UWPH_O_CREAT|UWPH_O_EXCL|UWPH_O_RDWR,0600);
    assert(fd==-1 && uwph_errno==UWPH_EEXIST);
    fd=uwph_open(path,UWPH_O_RDWR); assert(fd>=0);
    assert(uwph_ftruncate(fd,CHUNK-4)==0); size_is(fd,CHUNK-4);
    assert(uwph_pwrite(fd,data,8,CHUNK-4)==8); size_is(fd,CHUNK+4);
    assert(uwph_ftruncate(fd,0)==0); size_is(fd,0);
    assert(uwph_ftruncate(fd,3*CHUNK)==0); size_is(fd,3*CHUNK);
    assert(uwph_pread(fd,readback,8,CHUNK-4)==8);
    for(int i=0;i<8;i++) assert(!readback[i]);
    assert(uwph_close(fd)==0);
    fd=uwph_open(path,UWPH_O_RDWR|UWPH_O_TRUNC); assert(fd>=0); size_is(fd,0);
    assert(uwph_close(fd)==0);
    for(int i=0;i<4;i++) { snprintf(part,sizeof(part),"%s.uwphpart%d",path,i); unlink(part); }
    assert(rmdir(dir)==0);
    puts("test_bigfile: PASS (THROUGH-uwph Linux; 3 GiB sparse, boundary I/O, reopen, shrink/grow, read-only mmap; console UNCONFIRMED)");
    return 0;
}
