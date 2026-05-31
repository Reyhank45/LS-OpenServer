# ─────────────────────────────────────────────────────────────────────────────
#  LS-OpenServer — Root Makefile
#
#  Delegates build and install to per-package Makefiles found at:
#    src/sys/pkg/<name>/Makefile
#
#  To add a new package:
#    1. Create src/sys/pkg/<name>/
#    2. Copy a package Makefile template there
#    3. Drop .c files into the appropriate <dest>/ subdirectory
#    The root picks it up automatically — no changes here needed.
# ─────────────────────────────────────────────────────────────────────────────

# ── toolchain — exported so every sub-make inherits them ──────────────────────
export CC      := clang
export SYSROOT := $(CURDIR)/freebsd-sysroot
export CFLAGS  := --target=x86_64-pc-freebsd14 --sysroot=$(SYSROOT) \
                  -Wall -Wextra -O2 -MMD -MP
export LDFLAGS := -Wl,-dynamic-linker,/System/libexec/ld-elf.so.1 \
                  -Wl,-rpath,/System/lib

# ── directories — exported so every sub-make inherits them ────────────────────
export BUILD_DIR  := $(CURDIR)/build
export OBJ_DIR    := $(BUILD_DIR)/obj
export BIN_DIR    := $(BUILD_DIR)/bin
export ROOTFS_DIR := $(BUILD_DIR)/rootfs

ISO_DIR    := $(BUILD_DIR)/iso
OUTPUT_ISO := LS-OpenServer.iso

# ── discover packages ─────────────────────────────────────────────────────────
#  Any directory under src/sys/pkg/ that contains a Makefile is a package.
PKGS := $(sort $(patsubst %/Makefile,%,$(wildcard src/sys/pkg/*/Makefile)))

.PHONY: all bin rootfs iso clean $(PKGS)

all: iso

# ─────────────────────────────────────────────────────────────────────────────
#  1. BIN — delegate compilation and linking to each package Makefile
# ─────────────────────────────────────────────────────────────────────────────
bin: $(PKGS)
	@echo ""
	@echo "  ✓ All packages built."

$(PKGS):
	@echo ""
	@echo "  Package [$(@F)] — building"
	@$(MAKE) -C $@ --no-print-directory

# ─────────────────────────────────────────────────────────────────────────────
#  2. ROOTFS — set up directory tree, then ask each package to install itself
# ─────────────────────────────────────────────────────────────────────────────
rootfs: bin
	@echo ""
	@echo "  Rootfs  — assembling $(ROOTFS_DIR)/"
	@rm -rf $(ROOTFS_DIR)
	@mkdir -p \
	    $(ROOTFS_DIR)/System/etc     \
	    $(ROOTFS_DIR)/System/dev     \
	    $(ROOTFS_DIR)/System/tmp     \
	    $(ROOTFS_DIR)/System/sbin    \
	    $(ROOTFS_DIR)/System/bin     \
	    $(ROOTFS_DIR)/System/lib     \
	    $(ROOTFS_DIR)/System/libexec \
	    $(ROOTFS_DIR)/System/var/log

	@for p in $(PKGS); do \
	    $(MAKE) -C $$p install --no-print-directory; \
	done

	@echo "  Install [lib]  - libc.so.7, ld-elf.so.1"
	@cp $(SYSROOT)/lib/libc.so.7        $(ROOTFS_DIR)/System/lib/
	@cp $(SYSROOT)/libexec/ld-elf.so.1  $(ROOTFS_DIR)/System/libexec/
	@echo ""
	@echo "  ✓ Rootfs built at $(ROOTFS_DIR)/"

# ─────────────────────────────────────────────────────────────────────────────
#  3. ISO
# ─────────────────────────────────────────────────────────────────────────────
iso: rootfs
	@echo ""
	@echo "  ISO     — assembling $(ISO_DIR)/"
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/kernel

	@echo "  Copy    — kernel"
	@if [ -f "kernel" ]; then \
	    cp kernel $(ISO_DIR)/boot/kernel/kernel; \
	else \
	    echo "  Warning: kernel not found — skipping."; \
	fi

	@echo "  Copy    — loader.efi"
	@if [ -f "loader.efi" ]; then \
	    cp loader.efi $(ISO_DIR)/boot/loader.efi; \
	    if [ -d "../../LS-OpenServer/boot/lua" ];      then cp -r ../../LS-OpenServer/boot/lua      $(ISO_DIR)/boot/; fi; \
	    if [ -d "../../LS-OpenServer/boot/defaults" ]; then cp -r ../../LS-OpenServer/boot/defaults $(ISO_DIR)/boot/; fi; \
	    mkdir -p $(ISO_DIR)/EFI/BOOT; \
	    cp loader.efi $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI; \
	else \
	    echo "  Warning: loader.efi not found — skipping."; \
	fi

	@echo "  Pack    — mfsroot.iso (RAMFS)"
	@if [ -d "$(ROOTFS_DIR)/System" ]; then \
	    xorriso -as mkisofs -R -J -V "mfsroot" \
	        -o "$(ISO_DIR)/boot/kernel/mfsroot.iso" \
	        "$(ROOTFS_DIR)/" 2>&1 | sed 's/^/          /'; \
	else \
	    echo "  Warning: rootfs/System not found — skipping mfsroot."; \
	fi

	@echo "  Config  — loader.conf"
	@{ \
	    echo 'boot_multicons="YES"'; \
	    echo 'console="efi,comconsole"'; \
	    echo 'init_path="/System/sbin/init"'; \
	    echo 'mfsroot_load="YES"'; \
	    echo 'mfsroot_type="mfs_root"'; \
	    echo 'mfsroot_name="mfsroot.iso"'; \
	    echo 'vfs.root.mountfrom="cd9660:/dev/md0"'; \
	} >> $(ISO_DIR)/boot/loader.conf
	@echo 'hint.acpi.0.disabled="0"' > $(ISO_DIR)/boot/device.hints

	@echo "  Build   — EFI System Partition image"
	@rm -f efiboot.img
	@dd if=/dev/zero of=efiboot.img bs=1M count=16 status=none
	@mkfs.fat efiboot.img 2>/dev/null
	@mmd   -i efiboot.img ::EFI
	@mmd   -i efiboot.img ::EFI/BOOT
	@mcopy -i efiboot.img $(ISO_DIR)/boot/loader.efi ::EFI/BOOT/BOOTX64.EFI
	@mv efiboot.img $(ISO_DIR)/boot/

	@echo "  Pack    — $(OUTPUT_ISO)"
	@xorriso -as mkisofs -R -J -V "LS-OpenServer" \
	    --efi-boot boot/efiboot.img \
	    -efi-boot-part --efi-boot-image \
	    -no-emul-boot \
	    -isohybrid-gpt-basdat \
	    -o "$(OUTPUT_ISO)" "$(ISO_DIR)" 2>&1 | sed 's/^/          /'
	@echo ""
	@echo "  ✓ ISO ready: $(OUTPUT_ISO)"

# ─────────────────────────────────────────────────────────────────────────────
#  clean — delegate to each package then remove the build tree
# ─────────────────────────────────────────────────────────────────────────────
clean:
	@echo "  Clean   — packages"
	@for p in $(PKGS); do $(MAKE) -C $$p clean --no-print-directory; done
	@echo "  Clean   — removing $(BUILD_DIR)/, $(OUTPUT_ISO), efiboot.img"
	@rm -rf $(BUILD_DIR) $(OUTPUT_ISO) efiboot.img
	@echo "  ✓ Done."
