# Makefile for NoMount

ifeq ($(filter /%,$(src)),)
NM_REAL_PATH := $(realpath $(srctree)/$(src))
else
NM_REAL_PATH := $(realpath $(src))
endif

NOMOUNT_COMMITS := $(shell \
    d='$(NM_REAL_PATH)'; \
    while [ -n "$$d" ] && [ "$$d" != "/" ] && [ ! -d "$$d/.git" ]; do d=$$(dirname "$$d"); done; \
    if [ -d "$$d/.git" ] && git -C "$$d" remote get-url origin 2>/dev/null | grep -q 'nomount'; then \
        git -C "$$d" rev-list --count HEAD 2>/dev/null; \
    fi)

ifneq ($(NOMOUNT_COMMITS),)
ccflags-y += -DNOMOUNT_COMMIT_COUNT=\"$(NOMOUNT_COMMITS)\"
endif

obj-$(CONFIG_NOMOUNT) += nomount.o
ccflags-y += -std=gnu11 -Wno-declaration-after-statement -O3 -fno-stack-protector

ifeq ($(shell grep -qE '\(\*[[:space:]]*__get\)' $(srctree)/include/linux/xattr.h 2>/dev/null && echo 1 || echo 0),1)
ccflags-y += -DNM_HAS_XATTR__GET
endif
