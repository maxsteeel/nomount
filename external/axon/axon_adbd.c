#include "utils.h"
#include "prop_info.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <paths.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#define LOG_TAG "[axon][adbd]"

EXPORT int __android_log_is_debuggable() { return 1; }

struct prop_override {
  const char *name;
  const char *value;
};

// adbd sees these; detection apps see the globally-spoofed values from resetprop
static const struct prop_override overrides[] = {
    { "ro.secure",                  "0" },
    { "service.adb.root",          "1" },
    { "ro.boot.verifiedbootstate", "orange" },
    { "service.adb.tcp.port",      "5555" },
    { "persist.adb.tcp.port",      "5555" },
    { "sys.usb.config",            "mtp,adb" },
    { "persist.sys.usb.config",    "mtp,adb" },
    { "sys.usb.state",             "mtp,adb" },
    { "sys.usb.ffs.adb.ready",    "1" },
    { "persist.service.adb.enable", "1" },
    { "init.svc.adbd",            "running" },
};

#define NUM_OVERRIDES (sizeof(overrides) / sizeof(overrides[0]))

static const char* find_override(const char* name) {
  for (size_t i = 0; i < NUM_OVERRIDES; i++) {
    if (!strcmp(name, overrides[i].name))
      return overrides[i].value;
  }
  return NULL;
}

// Synthetic prop_info for properties that don't exist yet in shared memory.
// GetProperty() calls find() first — if NULL, it skips read_callback entirely.
struct fake_prop {
  prop_info pi;
  char name_buf[PROP_NAME_MAX];
};

static struct fake_prop fake_props[NUM_OVERRIDES];

static void init_fake_props(void) {
  klog(LOG_TAG, "init_fake_props: creating %zu synthetic prop_info entries", NUM_OVERRIDES);
  for (size_t i = 0; i < NUM_OVERRIDES; i++) {
    fake_props[i].pi.serial = 0;
    memset(fake_props[i].pi.value, 0, PROP_VALUE_MAX);
    strlcpy(fake_props[i].pi.name, overrides[i].name, PROP_NAME_MAX);
    klog(LOG_TAG, "  fake[%zu]: %s -> %s (addr=%p)",
         i, overrides[i].name, overrides[i].value, (void*)&fake_props[i]);
  }
}

typedef const prop_info* (*__system_property_find_t)(const char* name);
__system_property_find_t orig___system_property_find;
EXPORT const prop_info* __system_property_find(const char* name) {
  for (size_t i = 0; i < NUM_OVERRIDES; i++) {
    if (!strcmp(name, overrides[i].name)) {
      const prop_info* real = orig___system_property_find
          ? orig___system_property_find(name) : NULL;
      if (real) {
        klog(LOG_TAG, "find: [%s] exists in prop area (pi=%p)", name, (void*)real);
        return real;
      }
      klog(LOG_TAG, "find: [%s] NOT in prop area, returning synthetic (pi=%p)",
           name, (void*)&fake_props[i]);
      return (const prop_info*)&fake_props[i];
    }
  }
  return orig___system_property_find ? orig___system_property_find(name) : NULL;
}

// For Android 9+
typedef void (*callback_t)(void* cookie, const char* name, const char* value, uint32_t serial);
typedef void (*__system_property_read_callback_t)(const prop_info* pi, callback_t callback, void* cookie);
__system_property_read_callback_t orig___system_property_read_callback;
EXPORT void __system_property_read_callback(const prop_info* pi, callback_t callback, void* cookie) {
  const char* val = find_override(pi->name);
  if (unlikely(val)) {
    klog(LOG_TAG, "read_callback: [%s] -> \"%s\" (spoofed, pi=%p)", pi->name, val, (void*)pi);
    callback(cookie, pi->name, val, pi->serial);
    return;
  }
  if (likely(orig___system_property_read_callback))
    orig___system_property_read_callback(pi, callback, cookie);
}

// For Android 8+
typedef int (*__system_property_read_t)(const prop_info* pi, char* name, char* value);
__system_property_read_t orig___system_property_read;
EXPORT int __system_property_read(const prop_info* pi, char* name, char* value) {
  const char* val = find_override(pi->name);
  if (unlikely(val)) {
    klog(LOG_TAG, "read: [%s] -> \"%s\" (spoofed, pi=%p)", pi->name, val, (void*)pi);
    if (name) strlcpy(name, pi->name, PROP_NAME_MAX);
    strcpy(value, val);
    return (int)strlen(val);
  }
  if (likely(orig___system_property_read))
    return orig___system_property_read(pi, name, value);
  return 0;
}

// For Android 7+
typedef int (*__system_property_get_t)(const char* name, char* value);
__system_property_get_t orig___system_property_get;
EXPORT int __system_property_get(const char* name, char* value) {
  const char* val = find_override(name);
  if (unlikely(val)) {
    klog(LOG_TAG, "get: [%s] -> \"%s\" (spoofed)", name, val);
    strcpy(value, val);
    return (int)strlen(val);
  }
  if (likely(orig___system_property_get))
    return orig___system_property_get(name, value);
  return 0;
}

typedef int (*execle_t)(const char* path, const char* arg0, ...);
execle_t orig_execle;
EXPORT int execle(UNUSED const char* path, const char* arg0, ...) {
  va_list ap;
  va_start(ap, arg0);
  int argc = 1;
  const char *arg, *tmp;
  while ((tmp = va_arg(ap, char*))) {
    arg = tmp;
    argc++;
  }
  char** envp = va_arg(ap, char**);
  va_end(ap);

  const char* sh_path = _PATH_BSHELL;
  char shell[PROP_VALUE_MAX];
  shell[0] = 0;
  __system_property_get("persist.sys.adb.shell", shell);
  if (likely(shell[0] && access(shell, X_OK) == 0)) {
    sh_path = shell;
  }

  if (unlikely(!orig_execle)) {
    errno = EINVAL;
    return -1;
  }

  int ret = -1;
  if (likely(argc == 1 || argc == 2)) {
    ret = orig_execle(sh_path, sh_path, "-", NULL, envp);
  } else if (argc == 3) {
    ret = orig_execle(sh_path, sh_path, "-c", arg, NULL, envp);
  } else {
    errno = EINVAL;
  }

  return ret;
}

CONSTRUCTOR UNUSED void axon_adbd_main() {
  klog(LOG_TAG, "injected into adbd (pid=%d)", getpid());
  unsetenv("LD_PRELOAD");

  init_fake_props();

  void* libc = dlopen("libc.so", RTLD_NOW);
  if (!libc) {
    klog(LOG_TAG, "FATAL: dlopen(libc.so) failed: %s", dlerror());
    return;
  }
  klog(LOG_TAG, "libc.so handle: %p", libc);

  orig___system_property_find =
      (__system_property_find_t)dlsym(libc, "__system_property_find");
  orig___system_property_read_callback =
      (__system_property_read_callback_t)dlsym(libc, "__system_property_read_callback");
  orig___system_property_read = (__system_property_read_t)dlsym(libc, "__system_property_read");
  orig___system_property_get = (__system_property_get_t)dlsym(libc, "__system_property_get");
  orig_execle = (execle_t)dlsym(libc, "execle");

  klog(LOG_TAG, "resolved: find=%p read_cb=%p read=%p get=%p execle=%p",
       orig___system_property_find,
       orig___system_property_read_callback,
       orig___system_property_read,
       orig___system_property_get,
       orig_execle);

  if (!orig___system_property_find)
    klog(LOG_TAG, "WARNING: __system_property_find not resolved");
  if (!orig___system_property_read_callback)
    klog(LOG_TAG, "WARNING: __system_property_read_callback not resolved");
  if (!orig___system_property_get)
    klog(LOG_TAG, "WARNING: __system_property_get not resolved");
}
