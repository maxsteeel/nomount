#pragma once
#include <unistd.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define UNUSED __attribute__((unused))
#define CONSTRUCTOR __attribute__((constructor))
#define EXPORT __attribute__((visibility("default"), unused))

uintptr_t get_module_base(pid_t pid, const char* module);
size_t klog(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
