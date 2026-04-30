# NoMount - Kernel Integration

This section contains everything related to the integration of NoMount into the kernel. 

### **Structure**:

The NoMount integration is divided into two main components:

* Central Code (src/): Contains the central logic of NoMount (nomount.c) and its definitions (nomount.h). Everything is kept privatized within the file subsystem (fs/).
* Integration Patch (*.patch): Contains the hooks that must be applied in the kernel source for the correct functioning of NoMount.

### Integration:

1. Integrate NoMount Hooks:

If your source code corresponds to a modern kernel (>= 5.4), you can apply the provided patch:

```bash
cd <your_kernel_source>
patch -p1 --fuzz=3 < path/to/nomount/kernel/nomount_kernel_integration.patch
```
* Note: The --fuzz=3 parameter here is completely safe and ensures correct integration in 5.4-6.12.

 If the patch fails or you prefer to apply the hooks manually, see INTEGRATION.md.

2. Copy the necessary files:

Transfer the NoMount code (src/) to the fs directory (fs/) of your kernel:

```bash
cp path/to/nomount/kernel/src/*<your_kernel_source>/fs
```

3. Configure and compile NoMount:

Enable NoMount in your defconfig or via menuconfig:

```
CONFIG_NOMOUNT=y
```

Then compile your kernel as usual. If you followed the steps correctly, at the end of the compilation you will have a kernel with NoMount integrated!

