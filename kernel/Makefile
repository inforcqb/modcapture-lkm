# modcapture - loadable kernel module, built against Android DDK kernel headers.
#
#   make -C $KDIR M=$PWD ARCH=arm64 CC=clang LLVM=1 LLVM_IAS=1 modules
#
# Single source file, so `obj-m += modcapture.o` also fixes KBUILD_MODNAME to
# "modcapture" (the module name in /proc/modules and the prefix of every
# message this module prints).
#
# No private kernel headers are pulled in.  The only "special" access this
# module needs is load_module(), and it reaches it through register_kprobe(),
# which resolves the symbol NAME through the kernel's own kallsyms - so there
# is nothing to -I into, and no kallsyms_lookup_name() import either.

obj-m += modcapture.o

# The kernel's own build flags are strict about declarations after statements;
# the ELF/modinfo parser in modcapture.c is clearer with them.
ccflags-y += -Wno-declaration-after-statement

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
