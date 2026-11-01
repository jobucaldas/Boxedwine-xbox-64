/*
 * Linux x86-64 ABI as seen by Wine's Unix side: constants and struct layouts.
 *
 * Defined here instead of taken from the build host's headers because on the
 * Windows build the CRT's values differ (mingw ENOSYS is 40, Linux is 38;
 * long is 32-bit; struct stat has another layout). Wine's Unix side is written
 * for these Linux values. tests/abi_check.c static-asserts every value and
 * offset below against glibc on the Linux build.
 */
#ifndef UWPH_ABI_H
#define UWPH_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef int64_t  uwph_ssize_t;
typedef int64_t  uwph_off_t;
typedef int64_t  uwph_time_t;
typedef uint32_t uwph_mode_t;
typedef int32_t  uwph_pid_t;
typedef uint32_t uwph_uid_t;
typedef uint32_t uwph_gid_t;
typedef uint32_t uwph_socklen_t;
typedef uint64_t uwph_nfds_t;
typedef int32_t  uwph_clockid_t;

/* ---- errno ---- */
#define UWPH_EPERM            1
#define UWPH_ENOENT           2
#define UWPH_ESRCH            3
#define UWPH_EINTR            4
#define UWPH_EIO              5
#define UWPH_EBADF            9
#define UWPH_ECHILD          10
#define UWPH_EAGAIN          11
#define UWPH_ENOMEM          12
#define UWPH_EACCES          13
#define UWPH_EFAULT          14
#define UWPH_EBUSY           16
#define UWPH_EEXIST          17
#define UWPH_EXDEV           18
#define UWPH_ENOTDIR         20
#define UWPH_EISDIR          21
#define UWPH_EINVAL          22
#define UWPH_ENFILE          23
#define UWPH_EMFILE          24
#define UWPH_ENOTTY          25
#define UWPH_EFBIG          27
#define UWPH_ENOSPC          28
#define UWPH_ESPIPE          29
#define UWPH_EROFS           30
#define UWPH_EPIPE           32
#define UWPH_ERANGE          34
#define UWPH_ENAMETOOLONG    36
#define UWPH_ENOSYS          38
#define UWPH_ENOTEMPTY       39
#define UWPH_ELOOP           40
#define UWPH_ENODATA         61
#define UWPH_ETIME           62
#define UWPH_EOVERFLOW       75
#define UWPH_ENOTSOCK        88
#define UWPH_EMSGSIZE        90
#define UWPH_EPROTONOSUPPORT 93
#define UWPH_EOPNOTSUPP      95
#define UWPH_EAFNOSUPPORT    97
#define UWPH_EADDRINUSE      98
#define UWPH_ECONNRESET     104
#define UWPH_EISCONN        106
#define UWPH_ENOTCONN       107
#define UWPH_ETIMEDOUT      110
#define UWPH_ECONNREFUSED   111

/* ---- open / fcntl / *at ---- */
#define UWPH_O_RDONLY     00
#define UWPH_O_WRONLY     01
#define UWPH_O_RDWR       02
#define UWPH_O_ACCMODE    03
#define UWPH_O_CREAT      0100
#define UWPH_O_EXCL       0200
#define UWPH_O_NOCTTY     0400
#define UWPH_O_TRUNC      01000
#define UWPH_O_APPEND     02000
#define UWPH_O_NONBLOCK   04000
#define UWPH_O_DIRECTORY  0200000
#define UWPH_O_NOFOLLOW   0400000
#define UWPH_O_CLOEXEC    02000000
#define UWPH_O_PATH       010000000

#define UWPH_AT_FDCWD            (-100)
#define UWPH_AT_SYMLINK_NOFOLLOW 0x100
#define UWPH_AT_REMOVEDIR        0x200
#define UWPH_AT_EMPTY_PATH       0x1000

#define UWPH_F_DUPFD          0
#define UWPH_F_GETFD          1
#define UWPH_F_SETFD          2
#define UWPH_F_GETFL          3
#define UWPH_F_SETFL          4
#define UWPH_F_GETLK          5
#define UWPH_F_SETLK          6
#define UWPH_F_SETLKW         7
#define UWPH_F_DUPFD_CLOEXEC  1030
#define UWPH_FD_CLOEXEC       1

/* setlocale categories (glibc numbering; uwph_setlocale maps to the CRT) */
#define UWPH_LC_CTYPE         0
#define UWPH_LC_NUMERIC       1
#define UWPH_LC_TIME          2
#define UWPH_LC_COLLATE       3
#define UWPH_LC_MONETARY      4
#define UWPH_LC_MESSAGES      5
#define UWPH_LC_ALL           6

#define UWPH_SEEK_SET 0
#define UWPH_SEEK_CUR 1
#define UWPH_SEEK_END 2

#define UWPH_S_IFMT   0170000
#define UWPH_S_IFSOCK 0140000
#define UWPH_S_IFLNK  0120000
#define UWPH_S_IFREG  0100000
#define UWPH_S_IFDIR  0040000
#define UWPH_S_IFCHR  0020000
#define UWPH_S_IFIFO  0010000

/* ---- mmap ---- */
#define UWPH_PROT_NONE   0
#define UWPH_PROT_READ   1
#define UWPH_PROT_WRITE  2
#define UWPH_PROT_EXEC   4
#define UWPH_MAP_SHARED          0x01
#define UWPH_MAP_PRIVATE         0x02
#define UWPH_MAP_FIXED           0x10
#define UWPH_MAP_ANONYMOUS       0x20
#define UWPH_MAP_32BIT           0x40
#define UWPH_MAP_NORESERVE       0x4000
#define UWPH_MAP_FIXED_NOREPLACE 0x100000
#define UWPH_MAP_FAILED          ((void *)-1)
#define UWPH_MADV_NORMAL   0
#define UWPH_MADV_DONTNEED 4
#define UWPH_MS_ASYNC      1
#define UWPH_MS_INVALIDATE 2
#define UWPH_MS_SYNC       4

/* ---- signals ---- */
#define UWPH_SIGHUP   1
#define UWPH_SIGINT   2
#define UWPH_SIGQUIT  3
#define UWPH_SIGILL   4
#define UWPH_SIGTRAP  5
#define UWPH_SIGABRT  6
#define UWPH_SIGBUS   7
#define UWPH_SIGFPE   8
#define UWPH_SIGKILL  9
#define UWPH_SIGUSR1 10
#define UWPH_SIGSEGV 11
#define UWPH_SIGUSR2 12
#define UWPH_SIGPIPE 13
#define UWPH_SIGALRM 14
#define UWPH_SIGTERM 15
#define UWPH_SIGCHLD 17
#define UWPH_SIGCONT 18
#define UWPH_SIGSTOP 19
#define UWPH_SIGIO   29
#define UWPH_SIGSYS  31
#define UWPH_NSIG    65

#define UWPH_SA_SIGINFO   0x00000004
#define UWPH_SA_RESTORER  0x04000000
#define UWPH_SA_ONSTACK   0x08000000
#define UWPH_SA_RESTART   0x10000000
#define UWPH_SA_NODEFER   0x40000000
#define UWPH_SA_RESETHAND 0x80000000u

#define UWPH_SIG_BLOCK   0
#define UWPH_SIG_UNBLOCK 1
#define UWPH_SIG_SETMASK 2
#define UWPH_SIG_DFL ((void *)0)
#define UWPH_SIG_IGN ((void *)1)
#define UWPH_SS_ONSTACK 1
#define UWPH_SS_DISABLE 2

/* si_code values Wine's handlers look at */
#define UWPH_SEGV_MAPERR 1
#define UWPH_SEGV_ACCERR 2
#define UWPH_BUS_ADRALN  1
#define UWPH_ILL_ILLOPC  1
#define UWPH_FPE_INTDIV  1
#define UWPH_FPE_FLTDIV  3
#define UWPH_TRAP_BRKPT  1
#define UWPH_TRAP_TRACE  2
#define UWPH_SI_TKILL    (-6)

/* gregs indices (sys/ucontext.h, x86-64) */
enum {
    UWPH_REG_R8 = 0, UWPH_REG_R9, UWPH_REG_R10, UWPH_REG_R11, UWPH_REG_R12,
    UWPH_REG_R13, UWPH_REG_R14, UWPH_REG_R15, UWPH_REG_RDI, UWPH_REG_RSI,
    UWPH_REG_RBP, UWPH_REG_RBX, UWPH_REG_RDX, UWPH_REG_RAX, UWPH_REG_RCX,
    UWPH_REG_RSP, UWPH_REG_RIP, UWPH_REG_EFL, UWPH_REG_CSGSFS, UWPH_REG_ERR,
    UWPH_REG_TRAPNO, UWPH_REG_OLDMASK, UWPH_REG_CR2, UWPH_NGREG
};

/* x86 trap numbers (TRAP_sig in Wine's signal_x86_64.c) */
#define UWPH_TRAP_DIVIDE   0
#define UWPH_TRAP_DEBUG    1
#define UWPH_TRAP_BPTFLT   3
#define UWPH_TRAP_PRIVINFLT 6
#define UWPH_TRAP_PROTFLT 13
#define UWPH_TRAP_PAGEFLT 14
#define UWPH_TRAP_ARITHTRAP 16
#define UWPH_TRAP_ALIGNFLT 17
#define UWPH_TRAP_XMMFLT  19

/* ---- sockets ---- */
#define UWPH_AF_UNSPEC  0
#define UWPH_AF_UNIX    1
#define UWPH_AF_INET    2
#define UWPH_AF_INET6  10
#define UWPH_AF_NETLINK 16
#define UWPH_SOCK_STREAM    1
#define UWPH_SOCK_DGRAM     2
#define UWPH_SOCK_SEQPACKET 5
#define UWPH_SOCK_NONBLOCK  04000
#define UWPH_SOCK_CLOEXEC   02000000
#define UWPH_SOL_SOCKET 1
#define UWPH_SCM_RIGHTS 1
#define UWPH_SO_REUSEADDR 2
#define UWPH_SO_TYPE      3
#define UWPH_SO_ERROR     4
#define UWPH_SO_SNDBUF    7
#define UWPH_SO_RCVBUF    8
#define UWPH_SO_PASSCRED 16
#define UWPH_SO_PEERCRED 17
#define UWPH_MSG_PEEK         0x2
#define UWPH_MSG_CTRUNC       0x8
#define UWPH_MSG_DONTWAIT     0x40
#define UWPH_MSG_NOSIGNAL     0x4000
#define UWPH_MSG_CMSG_CLOEXEC 0x40000000
#define UWPH_SHUT_RD   0
#define UWPH_SHUT_WR   1
#define UWPH_SHUT_RDWR 2

/* ---- poll / epoll ---- */
#define UWPH_POLLIN   0x001
#define UWPH_POLLPRI  0x002
#define UWPH_POLLOUT  0x004
#define UWPH_POLLERR  0x008
#define UWPH_POLLHUP  0x010
#define UWPH_POLLNVAL 0x020
#define UWPH_POLLRDHUP 0x2000
#define UWPH_EPOLL_CTL_ADD 1
#define UWPH_EPOLL_CTL_DEL 2
#define UWPH_EPOLL_CTL_MOD 3
#define UWPH_EPOLLONESHOT (1u << 30)
#define UWPH_EPOLLET      (1u << 31)
#define UWPH_EPOLL_CLOEXEC UWPH_O_CLOEXEC

/* ---- clocks / futex / syscalls ---- */
#define UWPH_CLOCK_REALTIME          0
#define UWPH_CLOCK_MONOTONIC         1
#define UWPH_CLOCK_PROCESS_CPUTIME_ID 2
#define UWPH_CLOCK_THREAD_CPUTIME_ID 3
#define UWPH_CLOCK_MONOTONIC_RAW     4
#define UWPH_CLOCK_REALTIME_COARSE   5
#define UWPH_CLOCK_MONOTONIC_COARSE  6
#define UWPH_CLOCK_BOOTTIME          7

#define UWPH_FUTEX_WAIT          0
#define UWPH_FUTEX_WAKE          1
#define UWPH_FUTEX_WAIT_BITSET   9
#define UWPH_FUTEX_WAKE_BITSET  10
#define UWPH_FUTEX_PRIVATE_FLAG 128
#define UWPH_FUTEX_CLOCK_REALTIME 256

/* x86-64 syscall numbers Wine passes to syscall() (grep of wine-10.0
 * dlls/ntdll/unix, server, loader: futex, gettid, tgkill, tkill,
 * arch_prctl, utimensat). */
#define UWPH_NR_futex      202
#define UWPH_NR_gettid     186
#define UWPH_NR_tgkill     234
#define UWPH_NR_tkill      200
#define UWPH_NR_arch_prctl 158
#define UWPH_NR_utimensat  280

#define UWPH_ARCH_SET_GS 0x1001
#define UWPH_ARCH_SET_FS 0x1002
#define UWPH_ARCH_GET_FS 0x1003
#define UWPH_ARCH_GET_GS 0x1004

/* ---- sysconf / getauxval / rlimit / prctl ---- */
#define UWPH_SC_CLK_TCK            2
#define UWPH_SC_OPEN_MAX           4
#define UWPH_SC_PAGESIZE          30
#define UWPH_SC_NPROCESSORS_CONF  83
#define UWPH_SC_NPROCESSORS_ONLN  84
#define UWPH_SC_PHYS_PAGES        85
#define UWPH_AT_PAGESZ        6
#define UWPH_AT_HWCAP        16
#define UWPH_AT_RANDOM       25
#define UWPH_AT_HWCAP2       26
#define UWPH_AT_SYSINFO_EHDR 33
#define UWPH_AT_MINSIGSTKSZ  51
#define UWPH_RLIMIT_DATA   2
#define UWPH_RLIMIT_STACK  3
#define UWPH_RLIMIT_NOFILE 7
#define UWPH_RLIMIT_AS     9
#define UWPH_RLIM_INFINITY (~(uint64_t)0)
#define UWPH_PR_SET_NAME 15
#define UWPH_PR_GET_NAME 16

/* ---- structs (Linux x86-64 / glibc layout) ---- */
struct uwph_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct uwph_timeval  { int64_t tv_sec; int64_t tv_usec; };

struct uwph_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t  __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    struct uwph_timespec st_atim;
    struct uwph_timespec st_mtim;
    struct uwph_timespec st_ctim;
    int64_t  __unused[3];
};

struct uwph_statfs {
    int64_t  f_type;
    int64_t  f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t  f_fsid[2];
    int64_t  f_namelen;
    int64_t  f_frsize;
    int64_t  f_flags;
    int64_t  f_spare[4];
};

struct uwph_dirent {          /* glibc struct dirent on x86-64 (== dirent64) */
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};
#define UWPH_DT_UNKNOWN 0
#define UWPH_DT_DIR     4
#define UWPH_DT_REG     8
#define UWPH_DT_LNK    10

struct uwph_iovec { void *iov_base; size_t iov_len; };

struct uwph_msghdr {
    void              *msg_name;
    uwph_socklen_t     msg_namelen;
    struct uwph_iovec *msg_iov;
    size_t             msg_iovlen;
    void              *msg_control;
    size_t             msg_controllen;
    int                msg_flags;
};

struct uwph_cmsghdr {
    size_t cmsg_len;
    int    cmsg_level;
    int    cmsg_type;
    /* unsigned char cmsg_data[]; */
};
#define UWPH_CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define UWPH_CMSG_LEN(len)   (UWPH_CMSG_ALIGN(sizeof(struct uwph_cmsghdr)) + (len))
#define UWPH_CMSG_SPACE(len) (UWPH_CMSG_ALIGN(sizeof(struct uwph_cmsghdr)) + UWPH_CMSG_ALIGN(len))
#define UWPH_CMSG_DATA(c)    ((unsigned char *)(c) + UWPH_CMSG_ALIGN(sizeof(struct uwph_cmsghdr)))
#define UWPH_CMSG_FIRSTHDR(m) ((m)->msg_controllen >= sizeof(struct uwph_cmsghdr) ? \
                               (struct uwph_cmsghdr *)(m)->msg_control : (struct uwph_cmsghdr *)0)

struct uwph_sockaddr    { uint16_t sa_family; char sa_data[14]; };
struct uwph_sockaddr_un { uint16_t sun_family; char sun_path[108]; };

struct uwph_pollfd { int fd; short events; short revents; };

#if defined(__GNUC__)
# define UWPH_PACKED __attribute__((packed))
#else
# define UWPH_PACKED
#endif
struct uwph_epoll_event { uint32_t events; uint64_t data; } UWPH_PACKED;

struct uwph_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

struct uwph_rlimit { uint64_t rlim_cur; uint64_t rlim_max; };

struct uwph_sysinfo {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    char     _f[4];
};

typedef struct { uint64_t val[16]; } uwph_sigset_t;   /* glibc: 1024 bits */

typedef struct uwph_siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    int __pad;
    union {
        void *addr;                               /* si_addr: SIGSEGV/SIGBUS/SIGILL/SIGFPE */
        struct { int32_t pid; uint32_t uid; } kill; /* si_pid/si_uid: SI_TKILL */
        int _pad[28];
    } u;
} uwph_siginfo_t;

typedef struct uwph_stack { void *ss_sp; int ss_flags; size_t ss_size; } uwph_stack_t;

/* struct _libc_fpstate == FXSAVE image == Windows XMM_SAVE_AREA32 */
struct uwph_fpstate {
    uint16_t cwd, swd, ftw, fop;
    uint64_t rip, rdp;
    uint32_t mxcsr, mxcr_mask;
    struct { uint16_t significand[4], exponent, padding[3]; } st[8];
    struct { uint32_t element[4]; } xmm[16];
    uint32_t padding[24];
};

typedef struct uwph_mcontext {
    int64_t gregs[UWPH_NGREG];
    struct uwph_fpstate *fpregs;
    uint64_t __reserved1[8];
} uwph_mcontext_t;

typedef struct uwph_ucontext {
    uint64_t uc_flags;
    struct uwph_ucontext *uc_link;
    uwph_stack_t uc_stack;
    uwph_mcontext_t uc_mcontext;
    uwph_sigset_t uc_sigmask;
    struct uwph_fpstate __fpregs_mem;
    uint64_t __ssp[4];
} uwph_ucontext_t;

typedef void (*uwph_sighandler_t)(int);
typedef void (*uwph_sigaction_fn)(int, uwph_siginfo_t *, void *);

struct uwph_sigaction {
    union { uwph_sighandler_t handler; uwph_sigaction_fn sigaction; } u;   /* sa_handler / sa_sigaction */
    uwph_sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#endif
