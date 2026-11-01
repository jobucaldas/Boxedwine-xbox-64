/* Harness prototype: opt-in, bounded 1 GiB parts (strictly below 2 GB).
 * Included after raw syscall and private descriptor helpers. */
#define BIG_CHUNK (1LL << 30)
#define BIG_PARTS 1024
static struct big_file {
    int used, fd, dir, flags;
    char path[4096];
} big_files[128];
static struct big_file *big_find(int fd)
{
    for (unsigned i=0; i<128; i++)
        if (big_files[i].used && big_files[i].fd==fd) return &big_files[i];
    return NULL;
}
static int big_path(struct big_file *b, int part, char *out)
{
    int n=snprintf(out,4096,"%s.uwphpart%d",b->path,part);
    return n>=4096 ? -UWPH_ENAMETOOLONG : 0;
}
static long big_part(struct big_file *b, int part, int create)
{
    char path[4096]; int r=big_path(b,part,path);
    if(r) return r;
    return raw(SYS_openat,b->dir,PTR(path),
               (b->flags & UWPH_O_ACCMODE) | UWPH_O_CLOEXEC | (create ? UWPH_O_CREAT : 0),0600,0,0);
}
static long big_size(struct big_file *b)
{
    struct uwph_stat st;
    for(int i=0;i<BIG_PARTS;i++) {
        long fd=big_part(b,i,0);
        if(fd<0) return fd;
        long r=raw(SYS_fstat,fd,PTR(&st),0,0,0,0);
        raw(SYS_close,fd,0,0,0,0,0);
        if(r<0) return r;
        if(st.st_size<BIG_CHUNK) return i*BIG_CHUNK+st.st_size;
    }
    return -UWPH_EFBIG;
}
static long big_resize(struct big_file *b, long len)
{
    if(len<0) return -UWPH_EINVAL;
    if(len>=BIG_PARTS*BIG_CHUNK) return -UWPH_EFBIG;
    if((b->flags & UWPH_O_ACCMODE)==UWPH_O_RDONLY) return -UWPH_EBADF;
    int last=len/BIG_CHUNK;
    for(int i=0;i<=last;i++) {
        long fd=big_part(b,i,1); if(fd<0) return fd;
        long r=raw(SYS_ftruncate,fd,i==last ? len%BIG_CHUNK : BIG_CHUNK,0,0,0,0);
        raw(SYS_close,fd,0,0,0,0,0); if(r<0) return r;
    }
    for(int i=last+1;i<BIG_PARTS;i++) {
        char path[4096]; int r=big_path(b,i,path); if(r) return r;
        long rc=raw(SYS_unlinkat,b->dir,PTR(path),0,0,0,0);
        if(rc==-UWPH_ENOENT) break;
        if(rc<0) return rc;
    }
    return 0;
}
static long big_open(int dir, const char *path, int flags, unsigned mode, int *handled)
{
    const char *env=getenv("UWPH_BIGFILE_SPLIT");
    if(!env || strcmp(env,"1") || !path || !strncmp(path,"/proc/",6)) return 0;
#ifdef UWPH_HARNESS_NATIVE_FDS
    /* Native Wine descriptors cannot carry virtual split semantics yet. */
    return 0;
#endif
    struct big_file candidate={.dir=dir,.flags=flags};
    if(strlen(path)>sizeof(candidate.path)-32) { *handled=1; return -UWPH_ENAMETOOLONG; }
    strcpy(candidate.path,path);
    char first[4096]; big_path(&candidate,0,first);
    struct uwph_stat st;
    long exists=raw(SYS_newfstatat,dir,PTR(first),PTR(&st),0,0,0);
    if(exists<0) {
        if(exists!=-UWPH_ENOENT) { *handled=1; return exists; }
        long original=raw(SYS_newfstatat,dir,PTR(path),PTR(&st),0,0,0);
        if(original==0 || !(flags & UWPH_O_CREAT)) return 0;
        if(original!=-UWPH_ENOENT) { *handled=1; return original; }
    }
    *handled=1;
    if(flags & (UWPH_O_APPEND | UWPH_O_DIRECTORY)) return -UWPH_ENOSYS;
    struct big_file *b=NULL;
    for(unsigned i=0;i<128;i++) if(!big_files[i].used) { b=&big_files[i]; break; }
    if(!b) return -UWPH_EMFILE;
    /* Pin the relative namespace even if caller changes cwd/closes dirfd. */
    long pinned=raw(SYS_openat,dir,PTR("."),UWPH_O_RDONLY|UWPH_O_DIRECTORY|UWPH_O_CLOEXEC,0,0,0);
    if(pinned<0) return pinned;
    candidate.dir=pinned;
    long fd=raw(SYS_openat,pinned,PTR(first),flags,mode,0,0);
    if(fd<0) { raw(SYS_close,pinned,0,0,0,0,0); return fd; }
    candidate.fd=fd; candidate.used=1; *b=candidate;
    if(flags & UWPH_O_TRUNC) {
        long r=big_resize(b,0);
        if(r<0) { b->used=0; raw(SYS_close,fd,0,0,0,0,0); raw(SYS_close,pinned,0,0,0,0,0); return r; }
    }
    return fd;
}
static void big_close(int fd)
{
    struct big_file *b=big_find(fd);
    if(b) { raw(SYS_close,b->dir,0,0,0,0,0); b->used=0; }
}
static long big_io(struct big_file *b, void *buf, size_t len, long off, int write)
{
    if(off<0 || len>LONG_MAX || off>LONG_MAX-(long)len) return -UWPH_EINVAL;
    if((write && (b->flags&UWPH_O_ACCMODE)==UWPH_O_RDONLY) ||
       (!write && (b->flags&UWPH_O_ACCMODE)==UWPH_O_WRONLY)) return -UWPH_EBADF;
    if(!len) return 0;
    long size=big_size(b); if(size<0) return size;
    if(write && off+(long)len>size) {
        long r=big_resize(b,off+len); if(r<0) return r;
    }
    if(!write) { if(off>=size) return 0; if(len>(size_t)(size-off)) len=size-off; }
    size_t done=0;
    while(done<len) {
        long pos=off+done; size_t count=BIG_CHUNK-pos%BIG_CHUNK;
        if(count>len-done) count=len-done;
        long fd=big_part(b,pos/BIG_CHUNK,0); if(fd<0) return done ? (long)done : fd;
        long r=raw(write ? SYS_pwrite64 : SYS_pread64,fd,PTR((char *)buf+done),count,pos%BIG_CHUNK,0,0);
        raw(SYS_close,fd,0,0,0,0,0);
        if(r<=0) return done ? (long)done : r;
        done+=r;
    }
    return done;
}
static long big_call(int fd, void *buf, size_t len, long off, int write)
{
    WRLOCK(); int n=native(fd); struct big_file *b=big_find(n);
    long r=n<0 ? -UWPH_EBADF : b ? big_io(b,buf,len,off,write) :
        raw(write ? SYS_pwrite64 : SYS_pread64,n,PTR(buf),len,off,0,0);
    UNLOCK(); return result(r);
}
static long big_map(struct big_file *b, void *addr, size_t len, int prot, int flags, long off)
{
    if(prot!=UWPH_PROT_READ || (flags!=UWPH_MAP_PRIVATE && flags!=UWPH_MAP_SHARED)) return -UWPH_ENOSYS;
    long size=big_size(b);
    if(!len || off<0 || off%4096 || size<0 || off>size || len>(size_t)(size-off)) return -UWPH_EINVAL;
    long p=raw(SYS_mmap,PTR(addr),len,UWPH_PROT_READ|UWPH_PROT_WRITE,UWPH_MAP_PRIVATE|UWPH_MAP_ANONYMOUS,-1,0);
    if((unsigned long)p>=(unsigned long)-4095) return p;
    long r=big_io(b,(void *)p,len,off,0);
    if(r==(long)len) r=raw(SYS_mprotect,p,len,prot,0,0,0);
    else if(r>=0) r=-UWPH_EIO;
    if(r<0) { raw(SYS_munmap,p,len,0,0,0,0); return r; }
    return p;
}

/* Sequential operations share the anchor's offset; positioned I/O never moves it. */
static long big_generic(struct big_file *b, long nr, long arg, long len, long unused)
{
    if(nr==SYS_read || nr==SYS_write) {
        long off=raw(SYS_lseek,b->fd,0,1,0,0,0);
        if(off<0) return off;
        long r=big_io(b,(void *)arg,len,off,nr==SYS_write);
        if(r>0) raw(SYS_lseek,b->fd,off+r,0,0,0,0);
        return r;
    }
    if(nr==SYS_lseek) {
        long base=0;
        if(len==1) base=raw(SYS_lseek,b->fd,0,1,0,0,0);
        else if(len==2) base=big_size(b);
        else if(len!=0) return -UWPH_EINVAL;
        if(base<0) return base;
        if((arg>0 && base>LONG_MAX-arg) || (arg<0 && arg < -base)) return -UWPH_EINVAL;
        return raw(SYS_lseek,b->fd,base+arg,0,0,0,0);
    }
    return -UWPH_ENOSYS;
}
