# NoMount

> WARNING: This project is in beta and may contain bugs.

**NoMount** is a kernel-based file injection and path redirection framework for Android kernels.

Unlike traditional root solutions that rely on `mount --bind` (which pollutes `/proc/mounts`, changes mount namespaces, and is easily detected), NoMount operates **purely at the VFS (Virtual File System) layer**. It manipulates path resolution and directory iteration directly inside the kernel, making injections effective yet virtually invisible to userspace detection methods.

## Why NoMount?

Traditional methods (such a Magic Mount) modify the mount table. Anti-cheat and banking apps scan `/proc/self/mountinfo` to find these anomalies.

**NoMount changes the paradigm:**

1. **Zero Mounts:** No `mount()` syscalls are ever used. The mount table remains 100% stock.
2. **Visual Injection:** Uses advanced `readdir` hooking to make "new" files appear in read-only directories (like `/vendor`) without physically touching the partition.
3. **Permission Bypass:** Bypasses filesystem permission checks for injected files, preventing crashes without modifying file attributes.
4. **Linker-Friendly:** Includes "Selective Path Spoofing" to satisfy the Android Linker's namespace isolation (vital for injecting `.so` libraries).

## Key Features

* **Ghost Redirection:** Redirects a target path (e.g., `/vendor/etc/audio.conf`) to a source file in `/data`. The app reads your file, but believes it's reading the original.
* **Virtual File Injection:** Inject completely new files into system directories. They appear in `ls`, file managers, and `File.list()` calls thanks to kernel-level directory entry injection.
* **Security Context Bypass:** Hooks `inode_permission` to grant access to injected files regardless of their actual owner/group in `/data`.
* **UID Filtering:** Built-in "Invisibility Cloak". You can block specific UIDs (apps) from seeing any injections, effectively isolating them from the modification.

## Architecture

NoMount moves away from complex metadata spoofing and relies on **surgical VFS hooks**:

1. **`fs/nomount.c`**: The core logic hub. Handles the hash tables, rule resolution, and injection strategies.
2. **`fs/namei.c`**: Intercepts path lookups. If a rule exists, it quietly switches the path pointer to your custom file.
3. **`fs/readdir.c`**: The "Visual" engine. It intercepts directory listing calls (`getdents64`), injecting virtual entries into the buffer so apps "see" files that don't exist.
4. **`fs/d_path.c`**: Performs selective path masking, primarily to allow injected libraries to pass Android's Linker Namespace restrictions.

## Integration & Build

NoMount is designed as a drop-in kernel subsystem.

### 1. Apply the Patch

Apply the provided patch to your kernel source (Compatible with Linux 4.14 ~ 6.x):

```bash
cd your_kernel_source/
patch -p1 --fuzz=10 < nomount-kernel-5.4.patch

```

### 2. Configure

Enable the subsystem in your `defconfig` or via `menuconfig`:

```makefile
CONFIG_NOMOUNT=y

```

### 3. Compile

Build your kernel image as usual. NoMount adds negligible overhead (~1KB binary size).

## Usage (Userspace)

Control the subsystem using the `nm` binary.

| Command | Syntax | Description |
| --- | --- | --- |
| **Add Rule** | `nm add <virtual> <real>` | Inject `real` file at `virtual` path. |
| **Delete Rule** | `nm del <virtual>` | Remove a specific injection rule. |
| **Add UID Block** | `nm block <uid>` | Hide all injections from this UID. |
| **Del UID Block** | `nm unblock <uid>` | Allow this UID to see injections again. |
| **List Rules** | `nm list` | Show injected files |
| **Clear All** | `nm clear` | Flush all rules and blocks immediately. |
| **Version** | `nm ver` | Show the kernel subsystem version. |

### Examples

**Inject a custom library:**

```bash
# The system thinks libfoo.so is in /vendor, but it loads from /data
nm add /vendor/lib64/soundfx/libfoo.so /data/local/tmp/my_lib.so

```

**Replace a config file:**

```bash
# Instantly replace audio configs system-wide
nm add /vendor/etc/audio_effects.conf /data/adb/modules/my_mod/audio_effects.conf

```

**Hide root from a banking app:**

```bash
# App with UID 10256 will see the stock system, no injections
nm block 10256

```

## Special thanks:

-  **[HymoFS](https://github.com/Anatdx/HymoFS)**: Inspiration for this project.
-  **[A7mdwassa](https://github.com/A7mdwassa)**: Tester and contributor.
-  **[backslashxx](https://github.com/backslashxx)**: Code optimization.
-  **[KernelSU](https://github.com/tiann/KernelSU)**: Root solution.

## Disclaimer

**NoMount** is a powerful kernel modification tool intended for research and development. Modifying kernel behavior carries inherent risks, including system instability or data loss. The developers are not responsible for bricked devices or thermonuclear war.

