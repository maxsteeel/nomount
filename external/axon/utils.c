//
// Created by shaty uka on 2024/3/20.
//

#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

uintptr_t get_module_base(pid_t pid, const char* module) {
  char filename[32];
  char line[1024];
  sprintf(filename, "/proc/%d/maps", pid);
  FILE* fp = fopen(filename, "r");
  if (fp == NULL) return -1;

  uintptr_t addr = 0;
  char perms[5];
  while (fgets(line, sizeof(line), fp)) {
    sscanf(line, "%lx-%*lx %s %*s %*s %*d", &addr, perms);  // NOLINT(*-err34-c)
    if (strstr(line, module)) {
      break;
    }
  }

  fclose(fp);
  return addr;
}

size_t klog(const char* tag, const char* fmt, ...) {
  static int kmsg_fd = -1;
  if (unlikely(kmsg_fd == -1)) {
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (kmsg_fd == -1) return -1;
  }

  char buf[1024];
  size_t offset = 0;
  offset += strlcpy(buf, tag, sizeof(buf));
  offset += strlcpy(buf + offset, " ", sizeof(buf) - offset);
  va_list ap;
  va_start(ap, fmt);
  offset += vsnprintf(buf + offset, sizeof(buf) - offset, fmt, ap);
  va_end(ap);
  write(kmsg_fd, buf, offset);
  return offset;
}
