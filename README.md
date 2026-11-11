# NoMount

> **WARNING:** This project operates directly at the kernel VFS layer and is intended for research and development. Proceed with caution.

**NoMount** is a VFS (Virtual File System) path redirection framework for Android kernels.

Unlike traditional mount solutions like Magic Mount or OverlayFS that rely on modifying the mount table and polluting `/proc/mounts`, NoMount operates purely in RAM. Instead of making a real mount, it intercepts path resolution and directory iteration dynamically, making file injections completely transparent to the Android system and userspace apps without generating any mounts.

<details>
<summary><strong>View Screenshots</strong></summary>
<br>
<table>
  <tr>
    <td width="50%" align="center"><a href="assets/module_1.jpg"><img src="assets/module_1.jpg" width="100%" alt="Home Status"></a></td>
    <td width="50%" align="center"><a href="assets/module_2.jpg"><img src="assets/module_2.jpg" width="100%" alt="Modules List"></a></td>
  </tr>
  <tr>
    <td align="center"><sub><b>Home Status</b></sub></td>
    <td align="center"><sub><b>Modules List</b></sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><a href="assets/module_3.jpg"><img src="assets/module_3.jpg" width="100%" alt="Exclusions Empty"></a></td>
    <td width="50%" align="center"><a href="assets/module_4.jpg"><img src="assets/module_4.jpg" width="100%" alt="Exclusions Active"></a></td>
  </tr>
  <tr>
    <td align="center"><sub><b>Exclusions Empty</b></sub></td>
    <td align="center"><sub><b>Exclusions Active</b></sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><a href="assets/module_5.jpg"><img src="assets/module_5.jpg" width="100%" alt="App Selector"></a></td>
    <td width="50%" align="center"><a href="assets/module_6.jpg"><img src="assets/module_6.jpg" width="100%" alt="Manual UID Entry"></a></td>
  </tr>
  <tr>
    <td align="center"><sub><b>App Selector</b></sub></td>
    <td align="center"><sub><b>Manual Entry</b></sub></td>
  </tr>
  <tr>
    <td width="50%" align="center"><a href="assets/module_7.jpg"><img src="assets/module_7.jpg" width="100%" alt="Options"></a></td>
    <td width="50%" align="center"><a href="assets/module_8.jpg"><img src="assets/module_8.jpg" width="100%" alt="Installation"></a></td>
  </tr>
  <tr>
    <td align="center"><sub><b>Options</b></sub></td>
    <td align="center"><sub><b>Installation</b></sub></td>
  </tr>
</table>
</details>

## How it Works

When you set up a redirection, you provide two paths: the original file the system expects to find (e.g., `/vendor/etc/audio.conf`), and your modified file located elsewhere (e.g., `/data/local/tmp/mod.conf`).

Instead of applying global hooks in-kernel that affect the whole system, NoMount specifically modifies the directory operations of the target path directly in the RAM cache. When an application requests the original file, NoMount intercepts the call and creates a fake file representation in RAM. Once the application opens this file, NoMount internally opens your real modified file in the background and links them together. From that exact moment, any read, memory mapping (`mmap`), or data operation performed by the application is forwarded to your real file. The application reads the modified content naturally, completely unaware of the redirection.

NoMount also takes control of directory iteration. This allows you to inject entirely new files into existing system directories or completely hide existing ones (known as whiteouts). It also supports **opaque directories** (hiding everything in a directory except what you explicitly inject). When an app or a terminal command like `ls` tries to list the contents of a directory, NoMount dynamically injects the new entries or omits the hidden ones, reflecting the changes in file lists and system calls. 

Furthermore, NoMount includes a dedicated **UID filter**. Before any path interception occurs, NoMount checks the User ID (UID) of the application making the request. If the app's UID is flagged (such as a banking app or a root detector), NoMount simply steps aside and lets the kernel resolve the path normally. The isolated app will see the stock, untouched filesystem. This can be applied globally or scoped to specific UID ranges (e.g., Android's isolated processes).

This approach makes NoMount highly compatible with both read-only partitions (like `erofs`) and read-write partitions (like `ext4`), because the underlying partition is never modified. Furthermore, all communication between the userspace tool and the kernel is handled via the Linux keyring subsystem (`add_key`), completely bypassing the complications of traditional `/dev` nodes and IOCTL commands.

## How to Use It (Metamodule)

NoMount is distributed as a **Metamodule** for KernelSU (and forks) and APatch (and forks). This means it acts as a master module that dynamically intercepts and manages the mounting process of other standard modules in your system.

The NoMount module includes a built-in **WebUI** that allows you to easily control the kernel subsystem without touching the terminal:
* **Hot Load/Unload:** Enable or disable modules on the fly without rebooting.
* **App Exclusion (UID Isolation):** Select specific apps (like banking apps or anti-cheats) from a list. The WebUI extracts their UID and isolates them, ensuring they only see the stock, untouched filesystem.
* **Global Flush:** Instantly clear all redirection rules.

### Do I need a custom kernel?

Since NoMount operates at the VFS layer, it requires kernel-level integration. However, how you get this integration depends on your device:

* **Standard GKI Kernels (5.10+):** You **DO NOT** need to modify your kernel. The NoMount release ZIP includes pre-compiled LKMs (Loadable Kernel Modules) that will inject the VFS subsystem automatically upon flashing.
* **Legacy Kernels (< 5.10) and Strict Kernels:** If you are on an older kernel (e.g., 4.14, 4.19) or a strictly locked kernel (e.g., Sultan kernels with `CONFIG_INTEGRATED_MODULES=y`), the pre-compiled LKMs will not load. You will need to integrate NoMount directly into your kernel source or compile a specific LKM for your device.

For detailed instructions on how to patch your kernel source or compile a custom LKM, please read [kernel/README.md](kernel/README.md).

### How to Install the NoMount Metamodule

If your kernel already has NoMount integrated, or if you plan to use the included / compiled LKM, simply download the latest module from [Releases](https://github.com/maxsteeel/nomount/releases) or the [Nightly builds (dev branch)](https://nightly.link/maxsteeel/nomount/workflows/build/dev). Flash it via KernelSU/APatch and reboot your device.

After rebooting, just install your favorite modules (like audio mods or system tweaks) in KernelSU/APatch exactly as you normally would. NoMount's Metamodule will automatically intercept them at boot and use VFS injection instead of traditional mounts.

## How to Use It (CLI)

The subsystem is controlled via the `nm` binary. The CLI uses a simple command structure.

### Rule Commands
| Command | Description |
| --- | --- |
| `nm rule add <vpath> <rpath>` | Inject `rpath` file/directory at `vpath`. |
| `nm rule add --whiteout <vpath>` | Hide the file/directory at `vpath` (whiteout). |
| `nm rule add --opaque <vdir> <rdir>` | Hide the original contents of `vdir` and use `rdir` as the new base. |
| `nm rule del <vpath>` | Remove a specific injection rule. |
| `nm rule list` | Show currently active injection rules. |
| `nm rule list --json` | Show currently active injection rules in JSON format. |
| `nm rule clear` | Flush all rules immediately. |

### UID Scoping (Advanced Rules)
You can restrict any rule to specific User IDs or ranges. This is useful for injecting files only for certain apps, or bypassing redirection for anti-cheat processes.
| Flag | Description |
| --- | --- |
| `--uid <id>` | Apply the rule only to a specific UID. |
| `--uid-range <a>-<b>` | Apply the rule to a range of UIDs. |
| `--uid-range all` | Apply to all (this is the default behavior). |
| `--uid-excluded <c>-<d>`| Apply to all UIDs *except* the specified range. |

*Note: You can combine these flags with `add` or `del` commands.*

### UID Exceptions (Isolation)
Instead of scoping rules, you can globally isolate a UID so it sees the stock filesystem.
| Command | Description |
| --- | --- |
| `nm uid add <uid>` | Add an app UID to the exception list. |
| `nm uid del <uid>` | Remove an app UID from the exception list. |
| `nm uid list` | List all isolated UIDs. |
| `nm uid clear` | Clear all isolated UIDs. |
| `nm uid block_isolated on/off` | Toggle blocking for Android isolated processes (UIDs 90000-99999). |

### General Commands
| Command | Description |
| --- | --- |
| `nm clear all` | Flush all rules and UID blocks immediately. |
| `nm version` | Show the kernel subsystem version. |

### Examples

* **Basic file injection:**
```bash
# The system thinks libfoo.so is in /vendor, but it loads from /data
nm rule add /vendor/lib64/soundfx/libfoo.so /data/local/tmp/my_lib.so
```

* **Hide a file completely (Whiteout):**

```bash
# Make the system think this configuration file does not exist
nm rule add --whiteout /system/etc/hidden_config.xml
```

* **Hide modifications from a specific app (UID Isolation):**

```bash
# App with UID 10256 will see the untouched filesystem, bypassing detection
nm uid add 10256
```

* **Advanced: Inject a file only for a specific app:**

```bash
# Only UID 10123 will see the modified build.prop
nm rule add --uid 10123 /system/build.prop /data/local/tmp/my_build.prop
```

> **Note:** Because NoMount operates entirely in RAM, manual rules or modifications created via the CLI are lost upon reboot. The metamodule handles the persistent injection of your installed modules automatically during the boot process.

### Special Thanks

* **[Kasumi](https://github.com/Anatdx/Kasumi)**: Inspiration during first versions of this project.
* **[ZQZCC](https://github.com/ZQZCC)**: Great contributor to this project.
* **[backslashxx](https://github.com/backslashxx)**: Code optimization (binary side)
* **[KernelSU](https://github.com/tiann/KernelSU)**: Root solution.
* **All testers and contributors**: Thanks for testing this project and helping to make it stable!

### License

- Files under `kernel` directory are [GPL-2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
- All other parts except `kernel` directory are [GPL-3](https://www.gnu.org/licenses/gpl-3.0.html)

### Disclaimer

**NoMount** is a kernel modification tool intended for research and development. Modifying kernel behavior carries inherent risks. The developers are not responsible for bricked devices, data loss, or thermonuclear war.
