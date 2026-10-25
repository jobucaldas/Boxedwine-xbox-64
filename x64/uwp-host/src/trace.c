#include "internal.h"
static uint64_t counts[UWPH_TRACE_COUNT];
static const char *const names[] = {"snprintf","fopen","__isoc23_fscanf","fclose","pthread_getspecific","pthread_sigmask","feof","stat","write","pthread_mutex_lock","pthread_mutex_unlock","writev","getenv","asprintf","sigaddset","fcntl","setlocale","time","getuid","realpath","sendmsg","fstat","putenv","recvmsg","setsockopt","chdir","dladdr","execv","getrlimit","isatty","pipe2","setbuf","setrlimit","sigemptyset","sysconf","umask","__getdelim","access","connect","dlsym","dup","fchdir","fork","getcwd","getpid","lstat","mkdir","mprotect","nl_langinfo","opendir","prctl","pthread_key_create","pthread_mutex_init","pthread_mutexattr_destroy","pthread_mutexattr_init","pthread_mutexattr_settype","pthread_setspecific","secure_getenv","sigaction","sigaltstack","socket","socketpair","syscall","vsnprintf"};
void uwph_trace_hit(enum uwph_trace_symbol s)
{ __atomic_add_fetch(&counts[s], 1, __ATOMIC_RELAXED); }
uint64_t uwph_trace_count(enum uwph_trace_symbol s)
{ return (unsigned)s < UWPH_TRACE_COUNT ? __atomic_load_n(&counts[s], __ATOMIC_RELAXED) : 0; }
const char *uwph_trace_name(enum uwph_trace_symbol s)
{ return (unsigned)s < UWPH_TRACE_COUNT ? names[s] : NULL; }
