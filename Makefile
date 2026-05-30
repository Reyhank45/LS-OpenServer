CC = clang
SYSROOT = $(CURDIR)/freebsd-sysroot
CFLAGS = --target=x86_64-pc-freebsd14 --sysroot=$(SYSROOT) -Wall -Wextra -O2

BUILD_DIR = build
BIN_DIR = $(BUILD_DIR)/bin
ROOTFS_DIR = $(BUILD_DIR)/rootfs
ISO_DIR = $(BUILD_DIR)/iso
OUTPUT_ISO = LS-OpenServer.iso

TARGETS = $(BIN_DIR)/init $(BIN_DIR)/shell $(BIN_DIR)/utils $(BIN_DIR)/installer

.PHONY: all clean bin rootfs iso

all: iso

# 1. Compile Binaries
bin: $(TARGETS)

$(BIN_DIR)/init: src/init.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN_DIR)/shell: src/shell.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN_DIR)/utils: src/utils.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN_DIR)/installer: src/installer.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

# 2. Build Rootfs
rootfs: bin
	@echo "Building root filesystem structure in $(ROOTFS_DIR)..."
	@rm -rf $(ROOTFS_DIR)
	@mkdir -p $(ROOTFS_DIR)/dev
	@mkdir -p $(ROOTFS_DIR)/System/etc
	@mkdir -p $(ROOTFS_DIR)/System/dev
	@mkdir -p $(ROOTFS_DIR)/System/tmp
	@mkdir -p $(ROOTFS_DIR)/System/sbin
	@mkdir -p $(ROOTFS_DIR)/System/bin
	@mkdir -p $(ROOTFS_DIR)/lib
	@mkdir -p $(ROOTFS_DIR)/libexec
	
	@cp $(BIN_DIR)/init $(ROOTFS_DIR)/System/sbin/init
	@cp $(BIN_DIR)/shell $(ROOTFS_DIR)/System/bin/lssh
	@cp $(BIN_DIR)/utils $(ROOTFS_DIR)/System/bin/lsutils
	@cp $(BIN_DIR)/installer $(ROOTFS_DIR)/System/sbin/installer
	
	@cd $(ROOTFS_DIR)/System/bin && ln -sf lsutils cdl && ln -sf lsutils cat && ln -sf lsutils mkdir && ln -sf lsutils echo
	
	@cp $(SYSROOT)/lib/libc.so.7 $(ROOTFS_DIR)/lib/
	@cp $(SYSROOT)/libexec/ld-elf.so.1 $(ROOTFS_DIR)/libexec/
	@echo "Root filesystem built at $(ROOTFS_DIR)/"

# 3. Build ISO
iso: rootfs
	@echo "Cleaning up old iso directory..."
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/kernel
	
	@echo "Copying Kernel..."
	@if [ -f "kernel" ]; then cp kernel $(ISO_DIR)/boot/kernel/kernel; else echo "Warning: kernel not found."; fi
	
	@echo "Copying loader.efi and lua scripts..."
	@if [ -f "loader.efi" ]; then \
		cp loader.efi $(ISO_DIR)/boot/loader.efi; \
		if [ -d "../../LS-OpenServer/boot/lua" ]; then cp -r ../../LS-OpenServer/boot/lua $(ISO_DIR)/boot/; fi; \
		if [ -d "../../LS-OpenServer/boot/defaults" ]; then cp -r ../../LS-OpenServer/boot/defaults $(ISO_DIR)/boot/; fi; \
		mkdir -p $(ISO_DIR)/EFI/BOOT; \
		cp loader.efi $(ISO_DIR)/EFI/BOOT/BOOTX64.EFI; \
	else \
		echo "Warning: loader.efi not found."; \
	fi
	
	@echo "Creating RAMFS (mfsroot) image..."
	@if [ -d "$(ROOTFS_DIR)/System" ]; then \
		xorriso -as mkisofs -R -J -V "mfsroot" -o "$(ISO_DIR)/boot/kernel/mfsroot.iso" "$(ROOTFS_DIR)/"; \
	else \
		echo "Warning: rootfs/System not found."; \
	fi
	
	@echo 'boot_multicons="YES"' >> $(ISO_DIR)/boot/loader.conf
	@echo 'console="efi,comconsole"' >> $(ISO_DIR)/boot/loader.conf
	@echo 'init_path="/System/sbin/init"' >> $(ISO_DIR)/boot/loader.conf
	@echo 'mfsroot_load="YES"' >> $(ISO_DIR)/boot/loader.conf
	@echo 'mfsroot_type="mfs_root"' >> $(ISO_DIR)/boot/loader.conf
	@echo 'mfsroot_name="mfsroot.iso"' >> $(ISO_DIR)/boot/loader.conf
	@echo 'vfs.root.mountfrom="cd9660:/dev/md0"' >> $(ISO_DIR)/boot/loader.conf
	@echo 'hint.acpi.0.disabled="0"' > $(ISO_DIR)/boot/device.hints
	
	@echo "Building EFI System Partition (ESP) image..."
	@rm -f efiboot.img
	@dd if=/dev/zero of=efiboot.img bs=1M count=16 status=none
	@mkfs.fat efiboot.img
	@mmd -i efiboot.img ::EFI
	@mmd -i efiboot.img ::EFI/BOOT
	@mcopy -i efiboot.img $(ISO_DIR)/boot/loader.efi ::EFI/BOOT/BOOTX64.EFI
	@mv efiboot.img $(ISO_DIR)/boot/
	
	@echo "Generating Pure FreeBSD UEFI ISO..."
	@xorriso -as mkisofs -R -J -V "LS-OpenServer" \
		-e boot/efiboot.img \
		-no-emul-boot \
		-isohybrid-gpt-basdat \
		-o "$(OUTPUT_ISO)" "$(ISO_DIR)"
	@if [ $$? -eq 0 ]; then echo "ISO successfully generated: $(OUTPUT_ISO)"; else echo "Failed to generate ISO."; exit 1; fi

clean:
	rm -rf $(BUILD_DIR) $(OUTPUT_ISO) efiboot.img
