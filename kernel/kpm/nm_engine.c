// SPDX-License-Identifier: GPL-2.0
#undef CONFIG_ARM64_LSE_ATOMICS

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/hashtable.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/xattr.h>
#include <linux/key-type.h>
#include <linux/shmem_fs.h>
#include <linux/highmem.h>
#include <linux/compat.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/jump_label.h>
#include <linux/srcu.h>
#include <linux/mm.h>
#include <asm/alternative.h>

#include "nm_kpm_shim.h"

#define NM_KPM_BUILD 1
#include "../src/nomount.c"

#undef memcpy
#undef memset
#undef memmove

void *memcpy(void *d, const void *s, __kernel_size_t n)
{
    char *dp = d; const char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
void *memset(void *d, int c, __kernel_size_t n)
{
    char *dp = d;
    while (n--) *dp++ = (char)c;
    return d;
}
void *memmove(void *d, const void *s, __kernel_size_t n)
{
    char *dp = d; const char *sp = s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n; sp += n;
    while (n--) *--dp = *--sp;
    return d;
}

void *__memcpy(void *d, const void *s, __kernel_size_t n) { return memcpy(d, s, n); }
void *__memset(void *d, int c, __kernel_size_t n) { return memset(d, c, n); }
void *__memmove(void *d, const void *s, __kernel_size_t n) { return memmove(d, s, n); }

#if defined(CONFIG_DEBUG_LIST) || defined(CONFIG_LIST_HARDENED)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
bool __list_add_valid_or_report(struct list_head *new, struct list_head *prev,
                                struct list_head *next)
#else
bool __list_add_valid(struct list_head *new, struct list_head *prev,
                      struct list_head *next)
#endif
{
    if (!nm_kpm_sym[NMS___list_add_valid_or_report])
        return true;
    return ((bool (*)(struct list_head *, struct list_head *, struct list_head *))
            nm_kpm_sym[NMS___list_add_valid_or_report])(new, prev, next);
}
#endif

void alt_cb_patch_nops(struct alt_instr *alt, __le32 *origptr,
                       __le32 *updptr, int nr_inst)
{
    if (!nm_kpm_sym[NMS_alt_cb_patch_nops])
        return;
    ((void (*)(struct alt_instr *, __le32 *, __le32 *, int))
     nm_kpm_sym[NMS_alt_cb_patch_nops])(alt, origptr, updptr, nr_inst);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
void __folio_put(struct folio *folio)
{
    ((void (*)(struct folio *))nm_kpm_sym[NMS___folio_put])(folio);
}
#else
void __put_page(struct page *page)
{
    ((void (*)(struct page *))nm_kpm_sym[NMS___folio_put])(page);
}
#endif

int nm_kpm_engine_init(void)
{
    return nomount_init();
}

void nm_kpm_engine_exit(void)
{
#ifdef NOMOUNT_HAS_EXIT
    nomount_exit();
#endif
}

long nm_kpm_engine_ctl(const char *args)
{
    (void)args;
    return 0;
}
