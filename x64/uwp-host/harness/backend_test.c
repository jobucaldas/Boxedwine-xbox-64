#include "uwph/uwph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>
static int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s (uwph_errno=%d)\n",__LINE__,#x,uwph_errno); exit(1); } } while (0)
static _Atomic int ready;
static int word;
static long waited;
static void *waiter(void *unused)
{
    struct uwph_timespec limit={2,0};
    atomic_store_explicit(&ready,1,memory_order_release);
    waited=uwph_futex(&word,UWPH_FUTEX_WAIT|UWPH_FUTEX_PRIVATE_FLAG,0,&limit,NULL,0);
    return NULL;
}
int main(void)
{
    char dir[]="/tmp/uwph-o1-XXXXXX", path[256], missing[256], buf[32]={0};
    struct uwph_stat st;
    struct uwph_timespec ts, shortwait={0,1000000};
    printf("backend=%s\n",uwph_backend_name());
    CHECK(!strcmp(uwph_backend_name(),"linux-passthrough"));
    CHECK(mkdtemp(dir)!=NULL); /* fixture setup only */
    snprintf(path,sizeof(path),"%s/input",dir);
    snprintf(missing,sizeof(missing),"%s/missing",dir);
    int fd=uwph_open(path,UWPH_O_CREAT|UWPH_O_EXCL|UWPH_O_RDWR,0600);
    CHECK(fd>=0x100000);
    CHECK(uwph_write(fd,"fixture contents\n",17)==17);
    CHECK(uwph_lseek(fd,0,0)==0);
    CHECK(uwph_read(fd,buf,7)==7 && !memcmp(buf,"fixture",7));
    CHECK(uwph_lseek(fd,0,1)==7);
    CHECK(uwph_pread(fd,buf,8,8)==8 && !memcmp(buf,"contents",8));
    CHECK(uwph_lseek(fd,0,1)==7);
    CHECK(uwph_pwrite(fd,"F",1,0)==1);
    CHECK(uwph_lseek(fd,0,1)==7);
    CHECK(uwph_pread(fd,buf,1,-1)==-1 && uwph_errno==UWPH_EINVAL);
    CHECK(uwph_fstat(fd,&st)==0 && st.st_size==17 && (st.st_mode&0777)==0600);
    CHECK(uwph_stat(path,&st)==0 && st.st_size==17);
    CHECK(uwph_lstat(path,&st)==0 && st.st_size==17);
    CHECK(uwph_open(missing,UWPH_O_RDONLY)==-1 && uwph_errno==UWPH_ENOENT);
    CHECK(uwph_stat(missing,&st)==-1 && uwph_errno==UWPH_ENOENT);
    CHECK(uwph_read(0,buf,1)==-1 && uwph_errno==UWPH_EBADF);
    CHECK(uwph_close(1)==-1 && uwph_errno==UWPH_EBADF);
    int d=uwph_open(dir,UWPH_O_RDONLY|UWPH_O_DIRECTORY);
    CHECK(d>=0x100000);
    int relative=uwph_openat(d,"input",UWPH_O_RDONLY);
    CHECK(relative>=0x100000);
    CHECK(uwph_fstatat(d,"input",&st,0)==0 && st.st_size==17);
    CHECK(uwph_close(relative)==0 && uwph_close(d)==0);
    char *p=uwph_mmap(NULL,4096,UWPH_PROT_READ,UWPH_MAP_PRIVATE,fd,0);
    CHECK(p!=UWPH_MAP_FAILED && !memcmp(p,"Fixture contents\n",17));
    CHECK(uwph_mprotect(p,4096,UWPH_PROT_READ|UWPH_PROT_WRITE)==0);
    p[0]='X'; CHECK(p[0]=='X');
    CHECK(uwph_pread(fd,buf,1,0)==1 && buf[0]=='F');
    CHECK(uwph_mprotect(p,4096,UWPH_PROT_NONE)==0);
    pid_t child=fork(); CHECK(child>=0); /* host-only protection observation */
    if(!child) { volatile char c=*p; (void)c; _exit(0); }
    int status; CHECK(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    CHECK(uwph_mprotect(p,4096,UWPH_PROT_READ)==0 && p[0]=='X');
    CHECK(uwph_munmap(p,4096)==0);
    unsigned char residency;
    CHECK(syscall(SYS_mincore,p,4096,&residency)==-1); /* host-only unmap observation */
    p=uwph_mmap(NULL,4096,UWPH_PROT_READ|UWPH_PROT_WRITE,UWPH_MAP_PRIVATE|UWPH_MAP_ANONYMOUS,-1,0);
    CHECK(p!=UWPH_MAP_FAILED && p[0]==0); p[0]=42;
    CHECK(uwph_munmap(p,4096)==0);
    CHECK(uwph_mmap(NULL,4096,UWPH_PROT_READ,UWPH_MAP_PRIVATE,0,0)==UWPH_MAP_FAILED && uwph_errno==UWPH_EBADF);
    CHECK(uwph_close(fd)==0);
    CHECK(uwph_close(fd)==-1 && uwph_errno==UWPH_EBADF);
    CHECK(uwph_read(fd,buf,1)==-1 && uwph_errno==UWPH_EBADF);
    CHECK(uwph_clock_gettime(UWPH_CLOCK_MONOTONIC,&ts)==0 && ts.tv_sec>0 && ts.tv_nsec<1000000000);
    CHECK(uwph_clock_getres(UWPH_CLOCK_MONOTONIC,&ts)==0);
    CHECK(uwph_clock_gettime(-123,&ts)==-1 && uwph_errno==UWPH_EINVAL);
    CHECK(uwph_nanosleep(&shortwait,NULL)==0);
    CHECK(uwph_gettid()==syscall(SYS_gettid));
    CHECK(uwph_futex(&word,UWPH_FUTEX_WAIT|UWPH_FUTEX_PRIVATE_FLAG,1,NULL,NULL,0)==-1 && uwph_errno==UWPH_EAGAIN);
    CHECK(uwph_futex(&word,UWPH_FUTEX_WAIT|UWPH_FUTEX_PRIVATE_FLAG,0,&shortwait,NULL,0)==-1 && uwph_errno==UWPH_ETIMEDOUT);
    pthread_t t; CHECK(pthread_create(&t,NULL,waiter,NULL)==0);
    while(!atomic_load_explicit(&ready,memory_order_acquire)) sched_yield();
    long woke=0;
    /* Wake count acknowledges kernel enrollment, not merely thread publication. */
    for(int i=0;i<10000 && !woke;i++) {
        woke=uwph_futex(&word,UWPH_FUTEX_WAKE|UWPH_FUTEX_PRIVATE_FLAG,1,NULL,NULL,0);
        if(!woke) uwph_nanosleep(&shortwait,NULL);
    }
    CHECK(woke==1);
    CHECK(pthread_join(t,NULL)==0 && waited==0);
    CHECK(uwph_futex(&word,999,0,NULL,NULL,0)==-1 && uwph_errno==UWPH_ENOSYS);
    CHECK(unlink(path)==0 && rmdir(dir)==0); /* fixture cleanup only */
    printf("checks: %d passed, 0 failed\nbackend tests: PASS\n",checks);
    puts("THROUGH uwph: file, memory, clock, gettid, typed futex.");
    puts("Host-only: fixture setup/cleanup, pthread coordination, protection/unmap observations.");
    puts("Stock Wine: none. Our launcher: not exercised.");
    return 0;
}
