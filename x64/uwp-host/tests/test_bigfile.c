/* THROUGH-uwph Linux prototype; no Wine or console execution claim. */
#include "uwph/uwph.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#define CHUNK (1LL<<30)
static void size_is(int fd, long long size)
{
    struct uwph_stat st;
    assert(uwph_fstat(fd,&st)==0 && st.st_size==size);
}
/* Exercise metadata and snapshots while a sequential write creates hundreds
 * of sparse parts. A separate handle also checks same-file coordination. */
struct growth {
    int fd;
    pthread_barrier_t start;
    atomic_int done;
};
static void *grow(void *arg)
{
    struct growth *g=arg;
    pthread_barrier_wait(&g->start);
    assert(uwph_write(g->fd,"X",1)==1);
    atomic_store(&g->done,1);
    return NULL;
}
static void concurrent_growth(int fd, const char *path)
{
    int reader=uwph_open(path,UWPH_O_RDONLY);
    assert(reader>=0);
    for(int round=0;round<3;round++) {
        assert(uwph_ftruncate(fd,4096)==0);
        assert(uwph_lseek(fd,999*CHUNK,UWPH_SEEK_SET)==999*CHUNK);
        struct growth g={.fd=fd,.done=ATOMIC_VAR_INIT(0)};
        assert(pthread_barrier_init(&g.start,NULL,2)==0);
        pthread_t worker;
        assert(pthread_create(&worker,NULL,grow,&g)==0);
        pthread_barrier_wait(&g.start);
        do {
            struct uwph_stat st;
            assert(uwph_fstat(reader,&st)==0);
            assert(st.st_size==4096 || st.st_size==999*CHUNK+1);
            void *map=uwph_mmap(NULL,4096,UWPH_PROT_READ,UWPH_MAP_PRIVATE,reader,0);
            assert(map!=UWPH_MAP_FAILED);
            for(int i=0;i<4096;i++) assert(!((unsigned char *)map)[i]);
            assert(uwph_munmap(map,4096)==0);
        } while(!atomic_load(&g.done));
        assert(pthread_join(worker,NULL)==0);
        assert(pthread_barrier_destroy(&g.start)==0);
        size_is(reader,999*CHUNK+1);
        char byte=0;
        assert(uwph_pread(reader,&byte,1,999*CHUNK)==1 && byte=='X');
    }
    assert(uwph_ftruncate(fd,0)==0);
    assert(uwph_close(reader)==0);
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
    concurrent_growth(fd,path);
    assert(uwph_close(fd)==0);
    for(int i=0;i<4;i++) { snprintf(part,sizeof(part),"%s.uwphpart%d",path,i); unlink(part); }
    assert(rmdir(dir)==0);
    puts("test_bigfile: PASS (THROUGH-uwph Linux; 3 GiB sparse, boundary I/O, reopen, shrink/grow, read-only mmap, concurrent sequential growth/metadata/snapshots; console UNCONFIRMED)");
    return 0;
}
