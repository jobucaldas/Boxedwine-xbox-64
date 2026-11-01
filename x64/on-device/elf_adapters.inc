/* Fixed-signature SysV -> Windows ABI adapters. See gen-elf-adapters.py. */
static uwph_FILE * SYSV a_fopen(const char *path, const char *mode) { return uwph_fopen(path,mode); }
static int SYSV a_fclose(uwph_FILE *f) { return uwph_fclose(f); }
static int SYSV a_feof(uwph_FILE *f) { return uwph_feof(f); }
static void SYSV a_setbuf(uwph_FILE *f, char *buf) { uwph_setbuf(f,buf); }
static uwph_ssize_t SYSV a___getdelim(char **line, size_t *n, int delim, uwph_FILE *f) { return uwph___getdelim(line,n,delim,f); }
static char * SYSV a_setlocale(int category, const char *locale) { return uwph_setlocale(category,locale); }
static void * SYSV a_mmap(void *addr, size_t len, int prot, int flags, int fd, uwph_off_t off) { return uwph_mmap(addr,len,prot,flags,fd,off); }
static int SYSV a_munmap(void *addr, size_t len) { return uwph_munmap(addr,len); }
static int SYSV a_mprotect(void *addr, size_t len, int prot) { return uwph_mprotect(addr,len,prot); }
static int * SYSV a___errno_location(void) { return uwph___errno_location(); }
static int SYSV a_sigaction(int sig, const struct uwph_sigaction *act, struct uwph_sigaction *old) { return uwph_sigaction(sig,act,old); }
static int SYSV a_sigemptyset(uwph_sigset_t *s) { return uwph_sigemptyset(s); }
static int SYSV a_sigaddset(uwph_sigset_t *s, int sig) { return uwph_sigaddset(s,sig); }
static int SYSV a_pthread_sigmask(int how, const uwph_sigset_t *set, uwph_sigset_t *old) { return uwph_pthread_sigmask(how,set,old); }
static int SYSV a_sigaltstack(const uwph_stack_t *ss, uwph_stack_t *old) { return uwph_sigaltstack(ss,old); }
static void * SYSV a_dlsym(void *handle, const char *name) { return uwph_dlsym(handle,name); }
static int SYSV a_dladdr(const void *addr, uwph_Dl_info *info) { return uwph_dladdr(addr,info); }
static int SYSV a_close(int fd) { return uwph_close(fd); }
static int SYSV a_dup(int fd) { return uwph_dup(fd); }
static int SYSV a_isatty(int fd) { return uwph_isatty(fd); }
static uwph_ssize_t SYSV a_read(int fd, void *buf, size_t len) { return uwph_read(fd,buf,len); }
static uwph_ssize_t SYSV a_write(int fd, const void *buf, size_t len) { return uwph_write(fd,buf,len); }
static uwph_ssize_t SYSV a_writev(int fd, const struct uwph_iovec *iov, int cnt) { return uwph_writev(fd,iov,cnt); }
static int SYSV a_pipe2(int fds[2], int flags) { return uwph_pipe2(fds,flags); }
static int SYSV a_stat(const char *path, struct uwph_stat *st) { return uwph_stat(path,st); }
static int SYSV a_lstat(const char *path, struct uwph_stat *st) { return uwph_lstat(path,st); }
static int SYSV a_fstat(int fd, struct uwph_stat *st) { return uwph_fstat(fd,st); }
static int SYSV a_access(const char *path, int mode) { return uwph_access(path,mode); }
static int SYSV a_mkdir(const char *path, uwph_mode_t mode) { return uwph_mkdir(path,mode); }
static uwph_ssize_t SYSV a_readlink(const char *path, char *buf, size_t len) { return uwph_readlink(path,buf,len); }
static char * SYSV a_realpath(const char *path, char *resolved) { return uwph_realpath(path,resolved); }
static char * SYSV a_getcwd(char *buf, size_t len) { return uwph_getcwd(buf,len); }
static int SYSV a_chdir(const char *path) { return uwph_chdir(path); }
static int SYSV a_fchdir(int fd) { return uwph_fchdir(fd); }
static uwph_mode_t SYSV a_umask(uwph_mode_t mask) { return uwph_umask(mask); }
static uwph_DIR * SYSV a_opendir(const char *path) { return uwph_opendir(path); }
static struct uwph_dirent * SYSV a_readdir(uwph_DIR *d) { return uwph_readdir(d); }
static int SYSV a_closedir(uwph_DIR *d) { return uwph_closedir(d); }
static uwph_pid_t SYSV a_getpid(void) { return uwph_getpid(); }
static uwph_uid_t SYSV a_getuid(void) { return uwph_getuid(); }
static long SYSV a_sysconf(int name) { return uwph_sysconf(name); }
static int SYSV a_getrlimit(int res, struct uwph_rlimit *rl) { return uwph_getrlimit(res,rl); }
static int SYSV a_setrlimit(int res, const struct uwph_rlimit *rl) { return uwph_setrlimit(res,rl); }
static int SYSV a_prctl(int op, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return uwph_prctl(op,a2,a3,a4,a5); }
static char * SYSV a_getenv(const char *name) { return uwph_getenv(name); }
static char * SYSV a_secure_getenv(const char *name) { return uwph_secure_getenv(name); }
static int SYSV a_putenv(char *s) { return uwph_putenv(s); }
static void SYSV a__exit(int code) { uwph__exit(code); }
static uwph_pid_t SYSV a_fork(void) { return uwph_fork(); }
static int SYSV a_execv(const char *path, char *const argv[]) { return uwph_execv(path,argv); }
static void * SYSV a_pthread_getspecific(uwph_pthread_key_t k) { return uwph_pthread_getspecific(k); }
static int SYSV a_pthread_setspecific(uwph_pthread_key_t k, const void *v) { return uwph_pthread_setspecific(k,v); }
static int SYSV a_pthread_mutexattr_init(uwph_pthread_mutexattr_t *a) { return uwph_pthread_mutexattr_init(a); }
static int SYSV a_pthread_mutexattr_destroy(uwph_pthread_mutexattr_t *a) { return uwph_pthread_mutexattr_destroy(a); }
static int SYSV a_pthread_mutexattr_settype(uwph_pthread_mutexattr_t *a, int type) { return uwph_pthread_mutexattr_settype(a,type); }
static int SYSV a_pthread_mutex_init(uwph_pthread_mutex_t *m, const uwph_pthread_mutexattr_t *a) { return uwph_pthread_mutex_init(m,a); }
static int SYSV a_pthread_mutex_lock(uwph_pthread_mutex_t *m) { return uwph_pthread_mutex_lock(m); }
static int SYSV a_pthread_mutex_unlock(uwph_pthread_mutex_t *m) { return uwph_pthread_mutex_unlock(m); }
static int SYSV a_socket(int domain, int type, int proto) { return uwph_socket(domain,type,proto); }
static int SYSV a_socketpair(int domain, int type, int proto, int sv[2]) { return uwph_socketpair(domain,type,proto,sv); }
static int SYSV a_connect(int fd, const struct uwph_sockaddr *addr, uwph_socklen_t len) { return uwph_connect(fd,addr,len); }
static uwph_ssize_t SYSV a_sendmsg(int fd, const struct uwph_msghdr *msg, int flags) { return uwph_sendmsg(fd,msg,flags); }
static uwph_ssize_t SYSV a_recvmsg(int fd, struct uwph_msghdr *msg, int flags) { return uwph_recvmsg(fd,msg,flags); }
static int SYSV a_setsockopt(int fd, int level, int name, const void *val, uwph_socklen_t len) { return uwph_setsockopt(fd,level,name,val,len); }
static uwph_time_t SYSV a_time(uwph_time_t *t) { return uwph_time(t); }
static const struct provider host_symbols[] = {
{"fopen",(void *)a_fopen},
{"fclose",(void *)a_fclose},
{"feof",(void *)a_feof},
{"setbuf",(void *)a_setbuf},
{"__getdelim",(void *)a___getdelim},
{"setlocale",(void *)a_setlocale},
{"mmap",(void *)a_mmap},
{"munmap",(void *)a_munmap},
{"mprotect",(void *)a_mprotect},
{"__errno_location",(void *)a___errno_location},
{"sigaction",(void *)a_sigaction},
{"sigemptyset",(void *)a_sigemptyset},
{"sigaddset",(void *)a_sigaddset},
{"pthread_sigmask",(void *)a_pthread_sigmask},
{"sigaltstack",(void *)a_sigaltstack},
{"dlsym",(void *)a_dlsym},
{"dladdr",(void *)a_dladdr},
{"close",(void *)a_close},
{"dup",(void *)a_dup},
{"isatty",(void *)a_isatty},
{"read",(void *)a_read},
{"write",(void *)a_write},
{"writev",(void *)a_writev},
{"pipe2",(void *)a_pipe2},
{"stat",(void *)a_stat},
{"lstat",(void *)a_lstat},
{"fstat",(void *)a_fstat},
{"access",(void *)a_access},
{"mkdir",(void *)a_mkdir},
{"readlink",(void *)a_readlink},
{"realpath",(void *)a_realpath},
{"getcwd",(void *)a_getcwd},
{"chdir",(void *)a_chdir},
{"fchdir",(void *)a_fchdir},
{"umask",(void *)a_umask},
{"opendir",(void *)a_opendir},
{"readdir",(void *)a_readdir},
{"closedir",(void *)a_closedir},
{"getpid",(void *)a_getpid},
{"getuid",(void *)a_getuid},
{"sysconf",(void *)a_sysconf},
{"getrlimit",(void *)a_getrlimit},
{"setrlimit",(void *)a_setrlimit},
{"prctl",(void *)a_prctl},
{"getenv",(void *)a_getenv},
{"secure_getenv",(void *)a_secure_getenv},
{"putenv",(void *)a_putenv},
{"_exit",(void *)a__exit},
{"fork",(void *)a_fork},
{"execv",(void *)a_execv},
{"pthread_getspecific",(void *)a_pthread_getspecific},
{"pthread_setspecific",(void *)a_pthread_setspecific},
{"pthread_mutexattr_init",(void *)a_pthread_mutexattr_init},
{"pthread_mutexattr_destroy",(void *)a_pthread_mutexattr_destroy},
{"pthread_mutexattr_settype",(void *)a_pthread_mutexattr_settype},
{"pthread_mutex_init",(void *)a_pthread_mutex_init},
{"pthread_mutex_lock",(void *)a_pthread_mutex_lock},
{"pthread_mutex_unlock",(void *)a_pthread_mutex_unlock},
{"socket",(void *)a_socket},
{"socketpair",(void *)a_socketpair},
{"connect",(void *)a_connect},
{"sendmsg",(void *)a_sendmsg},
{"recvmsg",(void *)a_recvmsg},
{"setsockopt",(void *)a_setsockopt},
{"time",(void *)a_time},
{NULL,NULL}};
