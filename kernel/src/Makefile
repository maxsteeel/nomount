# Makefile for NoMount

NM_REAL_PATH := $(shell readlink -f $(src))
NOMOUNT_COMMITS := $(shell git -C $(NM_REAL_PATH) rev-list --count HEAD 2>/dev/null)
ifneq ($(NOMOUNT_COMMITS),)
ccflags-y += -DNOMOUNT_COMMIT_COUNT=\"$(NOMOUNT_COMMITS)\"
endif

obj-$(CONFIG_NOMOUNT) += nomount.o
ccflags-y += -std=gnu11 -Wno-declaration-after-statement -O3 -fno-stack-protector
