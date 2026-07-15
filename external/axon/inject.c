#include "utils.h"
#include "ptrace.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#define LOG_TAG "[axon][inject]"

uintptr_t reg_stack_alloc(struct user_regs_struct* regs, size_t size) {
  size = ALIGN_UP(size, sizeof(long));
  regs->REG_SP -= size;
  return regs->REG_SP;
}

int inject(pid_t pid, const char* path) {
  klog(LOG_TAG, "pid: %d, path: %s\n", pid, path);

  Dl_info dl_info;
  if (dladdr(&dlopen, &dl_info) == 0) {
    klog(LOG_TAG, "can not find dlopen in injector process\n");
    return -1;
  }

  uintptr_t local_libdl_base = (uintptr_t)dl_info.dli_fbase;
  uintptr_t local_dlopen = (uintptr_t)&dlopen;
  uintptr_t remote_libdl_base = get_module_base(pid, "/libdl.so\n");
  if (remote_libdl_base == 0) {
    klog(LOG_TAG, "can not find libdl.so in target process\n");
    return -1;
  }
  uintptr_t offset_dlopen = local_dlopen - local_libdl_base;
  uintptr_t remote_dlopen = remote_libdl_base + offset_dlopen;
  klog(LOG_TAG, "remote libdl: %p\n", (void*)remote_libdl_base);
  klog(LOG_TAG, "remote dlopen: %p\n", (void*)remote_dlopen);

  int path_size = (int)strlen(path) + 1;
  int path_size_aligned = ALIGN_UP(path_size, sizeof(long));
  char path_aligned[PATH_MAX];
  memset(path_aligned, 0, PATH_MAX);
  strcpy(path_aligned, path);

  if (ptrace_attach(pid)) {
    klog(LOG_TAG, "failed to attach\n");
    return -1;
  }
  if (remote_wait(pid)) {
    klog(LOG_TAG, "remote_wait failed\n");
    return -1;
  }

  struct user_regs_struct old_regs, regs;
  if (ptrace_getregs(pid, &regs)) {
    klog(LOG_TAG, "failed to get registers\n");
    return -1;
  }
  memcpy(&old_regs, &regs, sizeof(struct user_regs_struct));

  uintptr_t remote_path = reg_stack_alloc(&regs, 1024);
  ptrace_write(pid, remote_path, path_aligned, path_size_aligned);

  uintptr_t args[2];
  args[0] = remote_path;
  args[1] = RTLD_NOW;
  if (remote_call(pid, &regs, remote_dlopen, 2, args, remote_libdl_base)) {
    return -1;
  }
  klog(LOG_TAG, "dlopen handle: %p\n", (void*)regs.REG_RET);

  int ret = 0;

  if (regs.REG_RET == 0) {
    ret = -1;

    uintptr_t offset_dlerror = (uintptr_t)&dlerror - local_libdl_base;
    uintptr_t remote_dlerror = remote_libdl_base + offset_dlerror;
    if (remote_call(pid, &regs, remote_dlerror, 0, NULL, 0)) {
      return -1;
    }
    if (regs.REG_RET) {
      char buf[1024];
      buf[0] = 0;
      ptrace_read(pid, regs.REG_RET, buf, 1024);
      klog(LOG_TAG, "dlerror: %s\n", buf);
    } else {
      klog(LOG_TAG, "dlerror: NULL\n");
    }
  }

  if (ptrace_setregs(pid, &old_regs)) {
    klog(LOG_TAG, "failed to restore registers\n");
    return -1;
  }
  if (ptrace_detach(pid)) {
    klog(LOG_TAG, "failed to detach\n");
    return -1;
  }

  return ret;
}

int main(int argc, UNUSED char* argv[]) {
  if (argc != 3) {
    klog(LOG_TAG, "usage: inject pid path\n");
    return -1;
  }

  pid_t opt_pid = (pid_t)strtol(argv[1], NULL, 10);
  const char* opt_path = argv[2];
  if (inject(opt_pid, opt_path)) {
    klog(LOG_TAG, "inject failed\n");
    return -1;
  }

  klog(LOG_TAG, "inject success\n");
  return 0;
}
