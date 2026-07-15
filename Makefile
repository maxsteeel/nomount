NDK_ROOT  ?= /opt/android-ndk-r25b
NDK_BIN   := $(NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin
CARGO     ?= $(shell command -v cargo 2>/dev/null || echo /home/president/.cargo/bin/cargo)
MODULE    := module

TARGETS := aarch64-linux-android armv7-linux-androideabi x86_64-linux-android i686-linux-android

ABI_aarch64-linux-android     := arm64-v8a
ABI_armv7-linux-androideabi   := armeabi-v7a
ABI_x86_64-linux-android      := x86_64
ABI_i686-linux-android        := x86

export PATH := $(NDK_BIN):$(PATH)

.PHONY: all debug release clean install-debug install-release check

all: debug release

debug:
	@for t in $(TARGETS); do \
		echo "==> [debug] $$t"; \
		$(CARGO) build --target $$t 2>&1; \
	done
	@echo "==> debug build complete (symbols preserved, no strip)"

release:
	@for t in $(TARGETS); do \
		echo "==> [release] $$t"; \
		$(CARGO) build --release --target $$t 2>&1; \
	done
	@echo "==> release build complete (LTO + stripped)"

install-debug:
	@for t in $(TARGETS); do \
		abi=$${ABI_$$t}; \
		src=target/$$t/debug/nomount; \
		if [ -f "$$src" ]; then \
			mkdir -p $(MODULE)/bin/$$abi; \
			cp "$$src" $(MODULE)/bin/$$abi/nomount; \
			echo "  $$abi <- $$src"; \
		fi; \
	done

install-release:
	@for t in $(TARGETS); do \
		abi=$${ABI_$$t}; \
		src=target/$$t/release/nomount; \
		if [ -f "$$src" ]; then \
			mkdir -p $(MODULE)/bin/$$abi; \
			cp "$$src" $(MODULE)/bin/$$abi/nomount; \
			echo "  $$abi <- $$src"; \
		fi; \
	done

check:
	@$(CARGO) check --target aarch64-linux-android

clean:
	$(CARGO) clean
