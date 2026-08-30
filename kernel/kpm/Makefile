ifndef TARGET_COMPILE
    $(error TARGET_COMPILE not set, e.g. aarch64-linux-android- or aarch64-linux-gnu-)
endif
ifndef KP_DIR
    $(error KP_DIR not set: checkout of KernelPatch / KPatch-Next-EXP)
endif
ifndef KERNEL_DIR
    $(error KERNEL_DIR not set: configured kernel source for the TARGET KMI (<= 6.6))
endif

CC ?= $(TARGET_COMPILE)gcc
LD ?= $(TARGET_COMPILE)ld

KP_INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include \
                   linux/tools/arch/arm64/include
KP_FLAGS := $(foreach d,$(KP_INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(d))

CC_INCLUDE := $(shell $(CC) -print-file-name=include)

K_FLAGS := -nostdinc -isystem $(CC_INCLUDE) -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/include/uapi \
           -I$(KERNEL_DIR)/include/generated -I$(KERNEL_DIR)/include/generated/uapi \
           -I$(KERNEL_DIR)/arch/arm64/include -I$(KERNEL_DIR)/arch/arm64/include/uapi \
           -I$(KERNEL_DIR)/arch/arm64/include/generated \
           -I$(KERNEL_DIR)/arch/arm64/include/generated/uapi \
           -include $(KERNEL_DIR)/include/linux/kconfig.h \
           -D__KERNEL__ -DMODULE -DKBUILD_MODNAME='"nomount"'

COMMON := -O2 -fno-stack-protector -fno-pic -ffreestanding -Wall

all: nomount.kpm

nomount.kpm: nm_kpm_entry.o nm_engine.o
	$(CC) -r -o $@ $^

nm_kpm_entry.o: nm_kpm_entry.c nm_kpm_syms.h
	$(CC) $(COMMON) $(KP_FLAGS) -c -o $@ $<

nm_engine.o: nm_engine.c nm_kpm_shim.h nm_kpm_syms.h ../src/nomount.c ../src/nomount.h
	$(CC) $(COMMON) $(K_FLAGS) -c -o $@ $<

.PHONY: undefined
undefined: nm_engine.o
	@$(TARGET_COMPILE)nm -u $< | awk '{print $$2}' | sort -u

.PHONY: clean
clean:
	rm -f *.o *.kpm
