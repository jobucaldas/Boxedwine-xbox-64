/*
 * uwph - POSIX host surface for Wine's Unix side inside a Dev Mode UWP app.
 *
 * Wine's ntdll.so / wineserver are written against a Linux-like libc. On Xbox
 * there is no Linux kernel, so this library provides that surface on top of
 * documented UWP-partition Win32 APIs. Every entry point keeps libc semantics:
 * it returns -1 (or MAP_FAILED / NULL) and sets uwph errno, using Linux x86-64
 * numbering for every constant and Linux x86-64 layout for every struct (see
 * abi.h). When Wine's Unix side is compiled against this host,
 * libc_names.h (generated from tools/surface.tsv) renames mmap -> uwph_mmap etc.
 *
 * Backends:
 *   UWPH_BACKEND_STUB   (Linux build, "for now"): host-dependent calls return
 *                       ENOSYS; portable logic (fd table, in-process sockets and
 *                       pipes, poll/epoll, dl registry, exception->signal mapping,
 *                       W^X policy, sysconf/uname) is real.
 *   UWPH_BACKEND_WIN32  (mingw-w64 build, APP partition headers): the parts that
 *                       are implemented on documented UWP APIs (memory via
 *                       *FromApp + codeGeneration, futex via WaitOnAddress, clocks,
 *                       threads, read-only files via CreateFile2, API probe).
 */
#ifndef UWPH_UWPH_H
#define UWPH_UWPH_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
# define UWPH_BACKEND_WIN32 1
#else
# define UWPH_BACKEND_STUB 1
#endif

/* Calling convention: the platform default on both builds (MS x64 on
 * Windows). Wine's Unix side is SysV on Linux only because its dispatchers
 * (__wine_syscall_dispatcher / __wine_unix_call_dispatcher in
 * signal_x86_64.c) remap the PE caller's MS registers to SysV. For the UWP
 * host the plan is to build the Unix side as ordinary MS-ABI code and drop
 * that remap, rather than -mabi=sysv: a SysV Unix side would need a SysV
 * C runtime too (CRT varargs/va_list cannot cross ABIs), and none exists for
 * Windows. See NOTES-x64.md, "Toolchain / ABI". */
#define UWPH_API

#define UWPH_VERSION_MAJOR 0
#define UWPH_VERSION_MINOR 1

#include "abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* errno: Linux numbering, per thread. Wine calls __errno_location(). */
UWPH_API int *uwph___errno_location(void);
#define uwph_errno (*uwph___errno_location())

/* One-time host init: fd table (0/1/2), signal dispatch, API probe.
 * local_root / install_root are the app's LocalFolder and package
 * InstallLocation (UTF-8, host path syntax); NULL = current directory. */
UWPH_API int uwph_init(const char *local_root, const char *install_root);

/* Name of the active backend, for logs. */
UWPH_API const char *uwph_backend_name(void);

#ifdef __cplusplus
}
#endif

#include "mem.h"
#include "thread.h"
#include "teb.h"
#include "signal.h"
#include "fd.h"
#include "fs.h"
#include "socket.h"
#include "poll.h"
#include "process.h"
#include "time.h"
#include "dl.h"
#include "syscall.h"
#include "server.h"
#include "probe.h"
#include "stdio.h"

#endif
