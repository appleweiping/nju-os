// User-space library for NJU-OS user programs on the AbstractMachine *native*
// backend.
//
// On `native`, a user process runs as ordinary host code inside its own
// AbstractMachine address space (a set of mmap'd pages).  A system call is
// issued by *reading the trap page* at address 0x100000, which is mapped
// PROT_NONE: the access raises SIGSEGV, the AM CTE turns it into an
// EVENT_SYSCALL, and the kernel's os->trap handler services it.  The System-V
// register convention passes the call number and arguments:
//
//     rax = number, rdi/rsi/rdx/rcx = arg0..arg3, rax = return value
//
// The faulting instruction `mov 0x100000, %eax` is exactly 7 bytes, matching
// the AM native SYSCALL_INSTR_LEN, so the kernel resumes right after it.

#ifndef ULIB_H
#define ULIB_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "../kernel/framework/syscall.h"
#include "../kernel/framework/user.h"

static inline long syscall(long num, long a0, long a1, long a2, long a3) {
  register long rax asm("rax") = num;
  register long rdi asm("rdi") = a0;
  register long rsi asm("rsi") = a1;
  register long rdx asm("rdx") = a2;
  register long rcx asm("rcx") = a3;
  asm volatile(
      "mov 0x100000, %%eax\n\t"   // 8b 04 25 00 00 10 00 -> traps: EVENT_SYSCALL
      : "+r"(rax)
      : "r"(rdi), "r"(rsi), "r"(rdx), "r"(rcx)
      : "memory");
  return rax;
}

// ------------------------------------------------------------ syscall wrappers
static inline int  kputc(char ch)                 { return syscall(SYS_kputc, ch, 0, 0, 0); }
static inline int  fork(void)                     { return syscall(SYS_fork, 0, 0, 0, 0); }
static inline int  _exit_(int status)             { return syscall(SYS_exit, status, 0, 0, 0); }
static inline int  wait(int *status)              { return syscall(SYS_wait, (long)status, 0, 0, 0); }
static inline int  kill(int pid)                  { return syscall(SYS_kill, pid, 0, 0, 0); }
static inline int  getpid(void)                   { return syscall(SYS_getpid, 0, 0, 0, 0); }
static inline int  sleep(int s)                   { return syscall(SYS_sleep, s, 0, 0, 0); }
static inline int64_t uptime(void)                { return syscall(SYS_uptime, 0, 0, 0, 0); }
static inline void *mmap(void *a, int n, int p, int f) { return (void*)syscall(SYS_mmap, (long)a, n, p, f); }
static inline int  exec(const char *path, char *const argv[]) { return syscall(SYS_exec, (long)path, (long)argv, 0, 0); }

// L4 file syscalls
static inline int  open(const char *path, int flags)     { return syscall(SYS_open, (long)path, flags, 0, 0); }
static inline int  read(int fd, void *buf, int n)         { return syscall(SYS_read, fd, (long)buf, n, 0); }
static inline int  write(int fd, const void *buf, int n)  { return syscall(SYS_write, fd, (long)buf, n, 0); }
static inline int  close(int fd)                          { return syscall(SYS_close, fd, 0, 0, 0); }
static inline int  lseek(int fd, int off, int whence)     { return syscall(SYS_lseek, fd, off, whence, 0); }
static inline int  fstat(int fd, void *st)                { return syscall(SYS_fstat, fd, (long)st, 0, 0); }
static inline int  dup(int fd)                            { return syscall(SYS_dup, fd, 0, 0, 0); }
static inline int  mkdir(const char *path)                { return syscall(SYS_mkdir, (long)path, 0, 0, 0); }
static inline int  link(const char *a, const char *b)     { return syscall(SYS_link, (long)a, (long)b, 0, 0); }
static inline int  unlink(const char *path)               { return syscall(SYS_unlink, (long)path, 0, 0, 0); }
static inline int  chdir(const char *path)                { return syscall(SYS_chdir, (long)path, 0, 0, 0); }

// open() flags (kept in sync with kernel vfs.h)
#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREATE 0x200
#define O_TRUNC  0x400

// lseek() whence
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// -------------------------------------------------------- minimal freestanding libc
static inline size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static inline void   puts_(const char *s)  { while (*s) kputc(*s++); }

// tiny printf supporting %d %x %c %s %p and %%; writes via kputc (SYS_kputc)
static inline void printf(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  char buf[32];
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') { kputc(*p); continue; }
    p++;
    switch (*p) {
      case 'd': {
        long v = va_arg(ap, int); unsigned long u; int neg = 0;
        if (v < 0) { neg = 1; u = (unsigned long)(-v); } else u = (unsigned long)v;
        int i = 0; if (u == 0) buf[i++] = '0';
        while (u) { buf[i++] = '0' + u % 10; u /= 10; }
        if (neg) kputc('-');
        while (i) kputc(buf[--i]);
        break;
      }
      case 'x': case 'p': {
        unsigned long u = (*p == 'p') ? (unsigned long)va_arg(ap, void*)
                                      : (unsigned long)va_arg(ap, unsigned int);
        if (*p == 'p') { kputc('0'); kputc('x'); }
        int i = 0; if (u == 0) buf[i++] = '0';
        while (u) { int d = u & 0xf; buf[i++] = d < 10 ? '0' + d : 'a' + d - 10; u >>= 4; }
        while (i) kputc(buf[--i]);
        break;
      }
      case 'c': kputc((char)va_arg(ap, int)); break;
      case 's': { const char *s = va_arg(ap, const char*); if (!s) s = "(null)"; puts_(s); break; }
      case '%': kputc('%'); break;
      default:  kputc('%'); if (*p) kputc(*p); break;
    }
  }
  va_end(ap);
}

#endif
