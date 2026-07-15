#include "ptrace.h"
#include "utils.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <linux/elf.h>
#include <linux/uio.h>
#include <time.h>

int remote_call(pid_t pid, struct user_regs_struct* regs, uintptr_t func_addr, int argc, const uintptr_t* argv,
                uintptr_t return_addr) {
  if (argc >= 1) regs->REG_ARG0 = argv[0];
  if (argc >= 2) regs->REG_ARG1 = argv[1];
  if (argc >= 3) regs->REG_ARG2 = argv[2];
  if (argc >= 4) regs->REG_ARG3 = argv[3];
  if (argc >= 5) regs->REG_ARG4 = argv[4];
  if (argc >= 6) regs->REG_ARG5 = argv[5];

  regs->REG_IP = func_addr;

#ifdef __x86_64__
  regs->REG_SP -= sizeof(long);
  ptrace_write(pid, regs->REG_SP, &return_addr, sizeof(return_addr));
#elif defined(__aarch64__)
  regs->regs[30] = return_addr;
#else
#  error "Not supported"
#endif

  if (ptrace_setregs(pid, regs)) return -1;
  if (ptrace_continue(pid)) return -1;
  if (remote_wait(pid)) return -1;
  if (ptrace_getregs(pid, regs)) return -1;

  return 0;
}

int remote_wait(pid_t pid) {
  while (1) {
    int status;
    pid_t err = waitpid(pid, &status, __WALL);
    if (err == -1) {
      if (errno == EINTR) continue;
      printf("waitpid(%d) failed: %s(%d)\n", pid, strerror(errno), errno);
      return -1;
    }
    if (!WIFSTOPPED(status)) {
      printf("target process not stopped\n");
      return -1;
    }
    return 0;
  }
}

int ptrace_attach(pid_t pid) {
  if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) {
    printf("ptrace(PTRACE_ATTACH) failed: %s(%d)\n", strerror(errno), errno);
    return -1;
  }
  return 0;
}

int ptrace_detach(pid_t pid) {
  if (ptrace(PTRACE_DETACH, pid, NULL, NULL) == -1) {
    printf("ptrace(PTRACE_DETACH) failed: %s(%d)\n", strerror(errno), errno);
    return -1;
  }
  return 0;
}

int ptrace_getregs(pid_t pid, struct user_regs_struct* regs) {
#ifdef __x86_64__
  if (ptrace(PTRACE_GETREGS, pid, NULL, regs) == -1) {
    printf("ptrace(PTRACE_GETREGS) failed: %s(%d)\n", strerror(errno), errno);
    return -1;
  }
#elif defined(__aarch64__)
  struct iovec iov;
  iov.iov_base = regs;
  iov.iov_len = sizeof(struct user_regs_struct);
  if (ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1) {
    printf("ptrace(PTRACE_GETREGSET) failed: %s(%d)\n", strerror(errno), errno);
    return -1;
  }
#endif
  return 0;
}

int ptrace_setregs(pid_t pid, struct user_regs_struct* regs) {
#ifdef __x86_64__
  if (ptrace(PTRACE_SETREGS, pid, NULL, regs) == -1) {
    fprintf(stderr, "ptrace(PTRACE_SETREGS) failed: %s(%d)\n", strerror(errno), errno);
    return false;
  }
#elif defined(__aarch64__)
  struct iovec iov;
  iov.iov_base = regs;
  iov.iov_len = sizeof(*regs);
  if (ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1) {
    printf("ptrace(PTRACE_SETREGSET) failed: %s(%d)\n", strerror(errno), errno);
    return -1;
  }
#endif
  return 0;
}

int ptrace_continue(pid_t pid) {
  if (ptrace(PTRACE_CONT, pid, NULL, NULL) == -1) {
    printf("ptrace(PTRACE_CONT) failed: %s(%d)\n", strerror(errno), errno);
    return -1;
  }
  return 0;
}

int ptrace_read(pid_t pid, uintptr_t addr, void* vptr, int len) {
  int count = 0;
  int i = 0;
  long word = 0;
  long* ptr = (long*)vptr;

  while (count < len) {
    errno = 0;
    word = ptrace(PTRACE_PEEKTEXT, pid, addr + count, NULL);
    if (unlikely(errno)) {
      printf("ptrace(PTRACE_PEEKTEXT) failed: %s(%d)\n", strerror(errno), errno);
      return -1;
    }
    count += sizeof(word);
    if (unlikely(count > len))
      memcpy(ptr + i, &word, sizeof(word) - (count - len));
    else
      ptr[i++] = word;
  }
  return 0;
}

int ptrace_write(pid_t pid, uintptr_t addr, const void* vptr, int len) {
  int count = 0;
  long word = 0;

  while (count < len) {
    memcpy(&word, (const char*)vptr + count, sizeof(word));
    word = ptrace(PTRACE_POKETEXT, pid, addr + count, word);
    if (unlikely(word == -1)) {
      printf("ptrace(PTRACE_POKETEXT) failed: %s(%d)\n", strerror(errno), errno);
      return -1;
    }
    count += sizeof(word);
  }
  return 0;
}
