QEMU       = qemu-system-x86_64
SHELL      = /bin/bash

# Directories
EDK2_DIR   = uefi
EDK_SETUP  = $(EDK2_DIR)/edksetup.sh
BOOT_PKG   = boot/LiteCoreBootManager.dsc
EDK2_BUILD = $(EDK2_DIR)/bin/boot/DEBUG_GCC5/X64/LiteCoreBootManager.efi

SRC_DIR    = src
SRC_BOOT   = boot
SRC_KERNEL = $(SRC_DIR)/kernel
SRC_USER   = $(SRC_DIR)/user
SRC_LIB    = $(SRC_DIR)/lib

OUT_DIR    = bin
K_OUT_DIR  = $(OUT_DIR)/kernel
B_OUT_DIR  = $(OUT_DIR)/boot
IMG_OUT_DIR = $(OUT_DIR)
README     = $(OUT_DIR)/README.txt

# Output files
BOOTX64    = $(B_OUT_DIR)/BOOTX64.EFI
KERNEL     = $(K_OUT_DIR)/kernel.bin
ESP_IMG    = $(IMG_OUT_DIR)/LiteCore.img
FS_IMG     = $(IMG_OUT_DIR)/fs.img

# QEMU flags
QEMU_FLAGS = -m 128M -serial stdio -display none -monitor none \
			-bios /usr/share/ovmf/OVMF.fd -d int,guest_errors -D qemu.log --no-reboot
QEMU_VGA   = 
CONSOLE    = -display curses

# Phony targets
.PHONY: all run run-console run-vga clean clean-all bootloader kernel user lib lib-build

.DEFAULT_GOAL := all

all: bootloader kernel user $(README) $(ESP_IMG) $(FS_IMG)

# Bootloader build
bootloader: $(BOOTX64)

$(BOOTX64): $(SRC_BOOT)/boot.c $(SRC_BOOT)/Boot.inf $(BOOT_PKG)
	@echo "Building LiteCoreBootManager..."
	@cd $(EDK2_DIR) && . edksetup.sh && build -p ../$(BOOT_PKG) -a X64 -t GCC5 -b DEBUG -q
	@mkdir -p $(B_OUT_DIR)
	@cp $(EDK2_BUILD) $(BOOTX64)
	@echo "created: $(BOOTX64)"

# Kernel build
kernel:
	@echo "==> Building kernel..."
	@$(MAKE) -C $(SRC_KERNEL)

# User programs build
user: lib
	@echo "==> Building user programs..."
	@$(MAKE) -C $(SRC_USER)

# Library setup/build
lib:
	@echo "==> Setting up library symlink..."
	@$(MAKE) -C $(SRC_USER) lib

lib-build:
	@echo "==> lib-build: building libc/newlib in $(SRC_LIB)"
	@if [ -d "$(SRC_LIB)" ]; then \
		if command -v x86_64-elf-gcc >/dev/null 2>&1 ; then \
			echo "Using cross toolchain x86_64-elf-*"; \
			cd $(SRC_LIB) && AR=x86_64-elf-ar RANLIB=x86_64-elf-ranlib CC=x86_64-elf-gcc CFLAGS= make -j$(shell nproc) || exit $$?; \
		else \
			echo "Cross toolchain x86_64-elf-* not found, attempting host build (may be incompatible)"; \
			cd $(SRC_LIB) && make -j$(shell nproc) || exit $$?; \
		fi; \
		$(MAKE) lib; \
	else \
		echo "$(SRC_LIB) not found, cannot build newlib"; exit 1; \
	fi

# README copy
$(README):
	@mkdir -p $(OUT_DIR)
	@cp README.md $(README)

# ESP image (UEFI boot partition)
$(ESP_IMG): $(BOOTX64) $(KERNEL)
	@rm -f $(ESP_IMG)
	@echo "Creating UEFI ESP image..."
	@dd if=/dev/zero of=$(ESP_IMG) bs=1M count=64 2>/dev/null
	@mkfs.vfat -F 32 $(ESP_IMG) >/dev/null 2>&1
	@mkdir -p $(OUT_DIR)/.mnt
	@sudo mount -o loop $(ESP_IMG) $(OUT_DIR)/.mnt
	@sudo mkdir -p $(OUT_DIR)/.mnt/EFI/BOOT
	@sudo cp $(BOOTX64) $(OUT_DIR)/.mnt/EFI/BOOT/BOOTX64.EFI
	@sudo cp $(KERNEL) $(OUT_DIR)/.mnt/kernel.bin
	@sudo umount $(OUT_DIR)/.mnt
	@rmdir $(OUT_DIR)/.mnt
	@echo "ESP image created: $(ESP_IMG)"

# Filesystem image (FAT16 with user programs and data)
$(FS_IMG): kernel user
	@rm -f $(FS_IMG)
	@echo "Creating FAT16 filesystem image..."
	@rm -rf bin/fs_tmp || true
	@mkdir -p bin/fs_tmp/kernel/fonts
	@mkdir -p bin/fs_tmp/usr
	@mkdir -p bin/fs_tmp/apps
	@mkdir -p bin/fs_tmp/lib
	@cp -f $(SRC_KERNEL)/fonts/ter-u12b.bdf bin/fs_tmp/kernel/fonts/ 2>/dev/null || true
	@rm -f bin/fs_tmp/usr/*.elf 2>/dev/null || true
	@rm -f bin/fs_tmp/apps/*.elf 2>/dev/null || true
	@if [ -d "bin/usr" ]; then \
		for f in bin/usr/*.elf; do \
			if [ -f "$$f" ]; then cp -f "$$f" bin/fs_tmp/usr/; fi; \
		done || true; \
	fi
	@if [ -d "bin/apps" ]; then \
		for f in bin/apps/*.elf; do \
			if [ -f "$$f" ]; then cp -f "$$f" bin/fs_tmp/apps/; fi; \
		done || true; \
	fi
	@cp -f bin/lib/* bin/fs_tmp/lib/ 2>/dev/null || true
	@find bin -type f \
		-not -name "*.o" \
		-not -name "fs.img" \
		-not -path "bin/fs_tmp/*" \
		-not -path "bin/usr/*" \
		-not -path "bin/user/*" \
		-exec bash -c 'dest="bin/fs_tmp/$${1#bin/}"; mkdir -p "$$(dirname "$$dest")"; cp "$$1" "$$dest"' _ {} \;
	@cp README.md bin/fs_tmp/README.md 2>/dev/null || true
	@python3 tools/mk_fat16_image.py $(FS_IMG) 256000 bin/fs_tmp
	@rm -rf bin/fs_tmp/fs_content
	@echo "FAT16 image created: $(FS_IMG)"

# QEMU run targets
run: $(ESP_IMG) $(FS_IMG)
	$(QEMU) $(QEMU_FLAGS) --drive file=$(ESP_IMG),format=raw -drive file=$(FS_IMG),format=raw,if=ide

run-console: $(ESP_IMG) $(FS_IMG)
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd $(CONSOLE) -drive file=$(ESP_IMG),format=raw -drive file=$(FS_IMG),format=raw,if=ide

run-vga: $(ESP_IMG) $(FS_IMG)
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd $(QEMU_VGA) -drive file=$(ESP_IMG),format=raw -drive file=$(FS_IMG),format=raw,if=ide -d int -D qemu.log --no-reboot -monitor stdio

# Clean targets
clean:
	@echo "Cleaning build artifacts..."
	@$(MAKE) -C $(SRC_KERNEL) clean
	@$(MAKE) -C $(SRC_USER) clean
	@rm -rf $(OUT_DIR)

clean-all: clean
	@echo "Cleaning all including EDK2..."
	@cd $(EDK2_DIR) && rm -rf bin/boot
	@rm -f $(ESP_IMG) $(FS_IMG)

fmt:
	@echo "Formatting..."
	@./tools/fmt.sh
	@echo "Done. :D"