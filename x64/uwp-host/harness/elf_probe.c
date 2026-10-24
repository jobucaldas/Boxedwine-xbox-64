#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"probe failure line %d: %s errno=%d\n",__LINE__,#x,errno); abort(); } } while(0)
static const char *fixture;
static void *worker(void *unused)
{
    (void)unused;
    for(int i=0;i<50;i++) {
        errno=E2BIG;
        int fd=open(fixture,O_RDONLY);
        CHECK(fd>=0 && errno==E2BIG);
        char buf[6]={0};
        CHECK(read(fd,buf,5)==5 && !strcmp(buf,"hello") && errno==E2BIG);
        void *p=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0);
        CHECK(p!=MAP_FAILED && !memcmp(p,"hello",5));
        CHECK(munmap(p,4096)==0);
        CHECK(close(fd)==0 && errno==E2BIG);
        p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(p!=MAP_FAILED); memset(p,0x5a,4096);
        CHECK(((unsigned char *)p)[4095]==0x5a && munmap(p,4096)==0);
    }
    return NULL;
}
int main(int argc,char **argv)
{
    CHECK(argc>=2); fixture=argv[1];
    char path[1024]; CHECK(snprintf(path,sizeof path,"%s.created",fixture)<(int)sizeof path);
    umask(0);
    int fd=open(path,O_RDONLY|O_CREAT|O_EXCL,0640);
    CHECK(fd>=0 && close(fd)==0);
    struct stat st; CHECK(stat(path,&st)==0 && (st.st_mode&0777)==0640);
    CHECK(unlink(path)==0);
    CHECK(open("/proc/self/uwph-O2-does-not-exist",O_RDONLY)==-1 && errno==ENOENT);
    char b;
    CHECK(read(-1,&b,1)==-1 && errno==EBADF);
    CHECK(close(-1)==-1 && errno==EBADF);
    CHECK(mmap(NULL,0,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==MAP_FAILED && errno==EINVAL);
    CHECK(munmap((void *)4096,0)==-1 && errno==EINVAL);
    /* Deliberate borrowed-fd control, classified separately from routing. */
    fd=syscall(SYS_openat,AT_FDCWD,"/dev/null",O_RDONLY,0);
    CHECK(fd>=0 && read(fd,&b,1)==0 && close(fd)==0);
    pthread_t threads[4];
    for(int i=0;i<4;i++) CHECK(pthread_create(&threads[i],NULL,worker,NULL)==0);
    for(int i=0;i<4;i++) CHECK(pthread_join(threads[i],NULL)==0);
    void (*report)(void)=dlsym(RTLD_DEFAULT,"uwph_shim_report");
    if(report && argc==2) {
        errno=ERANGE; report(); CHECK(errno==ERANGE);
    }
    puts("elf probe: PASS"); fflush(stdout);
    if(argc>2) _exit(0);
    return 0;
}
