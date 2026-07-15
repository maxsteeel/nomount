#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <plthook.h>

#define LOG_TAG "[axon][init]"

#define ADBD_PATH_APEX "/apex/com.android.adbd/bin/adbd"
#define ADBD_PATH "/system/bin/adbd"
#define AXON_ADBD "/data/adb/axon/libaxon_adbd.so"

int (*orig_execv)(const char* path, char* const argv[]);
int hook_execv(const char* path, char* const argv[]) {
  if (unlikely(!strcmp(path, ADBD_PATH_APEX) || !strcmp(path, ADBD_PATH))) {
    klog(LOG_TAG, "adbd pid: %d\n", getpid());
    setenv("LD_PRELOAD", AXON_ADBD, 1);
    return orig_execv(path, argv);
  }
  return orig_execv(path, argv);
}

CONSTRUCTOR UNUSED void axon_init_main() {
  klog(LOG_TAG, "injected into init");

  plthook_t* plthook;
  int err = plthook_open_by_address(&plthook, (void*)get_module_base(1, "/init\n"));
  klog(LOG_TAG, "plthook err: %d\n", err);
  if (err) return;
  plthook_replace(plthook, "execv", (void*)&hook_execv, (void**)&orig_execv);
}
