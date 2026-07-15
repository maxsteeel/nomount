#pragma once

#include <sys/user.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#ifdef __x86_64__
#  define REG_RET rax
#  define REG_ARG0 rdi
#  define REG_ARG1 rsi
#  define REG_ARG2 rdx
#  define REG_ARG3 rcx
#  define REG_ARG4 r8
#  define REG_ARG5 r9
#  define REG_IP rip
#  define REG_SP rsp
#elif defined(__aarch64__)
#  define REG_RET regs[0]
#  define REG_ARG0 regs[0]
#  define REG_ARG1 regs[1]
#  define REG_ARG2 regs[2]
#  define REG_ARG3 regs[3]
#  define REG_ARG4 regs[4]
#  define REG_ARG5 regs[5]
#  define REG_IP pc
#  define REG_SP sp
#else
#  error "Not supported"
#endif

#define ALIGN_UP(x, align) ((x + (align - 1)) & ~(align - 1))

int remote_call(pid_t pid, struct user_regs_struct* regs, uintptr_t func_addr, int argc, const uintptr_t* argv,
                uintptr_t return_addr);
int remote_wait(pid_t pid);

int ptrace_attach(pid_t pid);
int ptrace_detach(pid_t pid);
int ptrace_getregs(pid_t pid, struct user_regs_struct* regs);
int ptrace_setregs(pid_t pid, struct user_regs_struct* regs);
int ptrace_continue(pid_t pid);
int ptrace_read(pid_t pid, uintptr_t addr, void* vptr, int len);
int ptrace_write(pid_t pid, uintptr_t addr, const void* vptr, int len);
