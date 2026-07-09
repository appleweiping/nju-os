// User program entry trampoline for the AbstractMachine native backend.
//
// ucontext() starts the process here with rsp = top of the user stack.  We
// call main(), then issue the exit() system call with main's return value.
// A system call is a 7-byte read of the trap page (0x100000).

#include "../kernel/framework/syscall.h"

.globl _start
_start:
  xorq  %rbp, %rbp
  call  main
  movl  %eax, %edi            // arg0 = exit status
  movq  $SYS_exit, %rax
  mov   0x100000, %eax        // trap: EVENT_SYSCALL (exit never returns)
1:
  jmp   1b
