# NoMount - Kernel Integration

This section contains everything related to the integration of NoMount into the kernel. 

### **Structure**:

The NoMount integration is divided into two main components:

* Central Code (src/): Contains the central logic of NoMount (nomount.c) and its definitions (nomount.h). Everything is kept privatized within the file subsystem (fs/).
* Integration Patch (*.patch): Contains the hooks that must be applied in the kernel source for the correct functioning of NoMount.

### Integration:

1. Integrate NoMount Hooks:

Apply the patch corresponding to your kernel version, e.g, for kernels 5.10:

```bash
cd <your_kernel_source>
patch -p1 < path/to/nomount/kernel/nomount_5.10_kernel_integration.patch
```

If the patch fails or you prefer to integrate NoMount manually in your kernel, see [INTEGRATION.md](INTEGRATION.md).

2. Copy the necessary files:

Transfer the NoMount code (src/) to the fs directory (fs/) of your kernel:

```bash
cp path/to/nomount/kernel/src/* <your_kernel_source>/fs
```

3. Configure and compile NoMount:

Enable NoMount in your defconfig or via menuconfig:

```
CONFIG_NOMOUNT=y
```

Then compile your kernel as usual. If you followed the steps correctly, at the end of the compilation you will have a kernel with NoMount integrated!

