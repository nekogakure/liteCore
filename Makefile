CC         = gcc
LD         = ld
NASM       = nasm
QEMU       = qemu-system-x86_64
OBJCOPY    = objcopy
SHELL      = /bin/bash

EDK2_DIR   = uefi
EDK_SETUP  = $(EDK2_DIR)/edksetup.sh
BOOT_PKG   = boot/LiteCoreBootManager.dsc
EDK2_BUILD = $(EDK2_DIR)/bin/boot/DEBUG_GCC5/X64/LiteCoreBootManager.efi

SRC_DIR    = src
SRC_BOOT   = boot
SRC_KERNEL = $(SRC_DIR)/kernel
SRC_USER   = $(SRC_DIR)/user
SRC_LIB    = $(SRC_DIR)/lib
FONTS      = $(SRC_KERNEL)/fonts/ter-u12b.bdf
INCLUDE    = $(SRC_DIR)/kernel
OUT_DIR    = bin
K_OUT_DIR  = $(OUT_DIR)/kernel
B_OUT_DIR  = $(OUT_DIR)/boot
F_OUT_DIR  = $(K_OUT_DIR)/fonts
IMG_OUT_DIR = $(OUT_DIR)
README     = $(OUT_DIR)/README.txt

CFLAGS     = -O2 -Wimplicit-function-declaration -Wunused-but-set-variable -ffreestanding -m64 -c -Wall -Wextra -I$(INCLUDE) -mcmodel=large -mno-red-zone -fno-pic
LDFLAGS    = -m elf_x86_64 -z max-page-size=0x1000

NFLAGS     = -f bin
QEMU_FLAGS = -m 128M -serial stdio -display none -monitor none \
			-bios /usr/share/ovmf/OVMF.fd -d int,guest_errors -D qemu.log --no-reboot
QEMU_VGA   = 
CONSOLE    = -display curses

SOURCES    = $(shell find $(SRC_KERNEL) -name "*.c")
ASM_SOURCES = $(shell find $(SRC_KERNEL) -name "*.asm")
OBJECTS    = $(shell printf "%s\n" $(patsubst $(SRC_KERNEL)/%.c, $(K_OUT_DIR)/%.o, $(SOURCES)) $(patsubst $(SRC_KERNEL)/%.asm, $(K_OUT_DIR)/%.o, $(ASM_SOURCES)) | sort -u)

USER_SOURCES = $(shell find $(SRC_USER) -name "*.c" \! -name "syscall.c" \! -name "stdio_stub.c")
USER_OBJECTS = $(shell printf "%s\n" $(patsubst $(SRC_USER)/%.c, $(OUT_DIR)/usr/%.o, $(USER_SOURCES)) | sort -u)
USER_ELFS = $(shell printf "%s\n" $(patsubst $(SRC_USER)/%.c, $(OUT_DIR)/usr/%.elf, $(USER_SOURCES)) | sort -u)

CRT_SRC = $(SRC_USER)/crt.asm

ifeq ($(wildcard apps),apps)
SRC_APPS = apps
else
SRC_APPS = $(SRC_DIR)/apps
endif
APP_OUT_DIR = $(OUT_DIR)/apps
APP_SOURCES = $(shell if [ -d "$(SRC_APPS)" ]; then find $(SRC_APPS) -name "*.c"; fi)
APP_OBJECTS = $(shell printf "%s\n" $(patsubst $(SRC_APPS)/%.c, $(APP_OUT_DIR)/%.o, $(APP_SOURCES)) | sort -u)
APP_ELFS = $(shell printf "%s\n" $(patsubst $(SRC_APPS)/%.c, $(APP_OUT_DIR)/%.elf, $(APP_SOURCES)) | sort -u)

ALL_USER_ELFS = $(USER_ELFS) $(APP_ELFS)

BIN_LIB_DIR = $(OUT_DIR)/lib
USER_LDFLAGS ?= -L$(BIN_LIB_DIR) -lc

BOOTX64    = $(B_OUT_DIR)/BOOTX64.EFI
KERNEL_ELF = $(K_OUT_DIR)/kernel.elf
KERNEL     = $(K_OUT_DIR)/kernel.bin
ESP_IMG    = $(IMG_OUT_DIR)/LiteCore.img
EXT2_IMG   = $(IMG_OUT_DIR)/fs.img
LINKER     = $(SRC_DIR)/kernel.ld
ESP_DIR    = esp

.PHONY: all run run-console run-vga clean bootloader kernel user lib ext2
.DEFAULT_GOAL := all

.PHONY: lib-build

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
		$(MAKE) -C $(CURDIR) lib; \
	else \
		echo "$(SRC_LIB) not found, cannot build newlib"; exit 1; \
	fi


all: bootloader kernel user $(F_OUT_DIR)/ter-u12b.bdf $(README) $(ESP_IMG) $(EXT2_IMG)

$(K_OUT_DIR):
	@mkdir -p $(K_OUT_DIR)

bootloader: $(BOOTX64)

$(BOOTX64): $(SRC_BOOT)/boot.c $(SRC_BOOT)/Boot.inf $(BOOT_PKG)
	@echo "Building LiteCoreBootManager..."
	@cd $(EDK2_DIR) && . edksetup.sh && build -p ../$(BOOT_PKG) -a X64 -t GCC5 -b DEBUG -q
	@mkdir -p $(B_OUT_DIR)
	@cp $(EDK2_BUILD) $(BOOTX64)
	@echo "created: $(BOOTX64)"

kernel: $(KERNEL)

$(KERNEL_ELF): $(K_OUT_DIR) $(OBJECTS) $(LINKER)
	$(LD) $(LDFLAGS) -T $(LINKER) $(OBJECTS) -o $@

$(KERNEL): $(KERNEL_ELF)
	@$(OBJCOPY) -O binary $< $@
	@echo "Kernel size: $$(wc -c < $@) bytes"

$(K_OUT_DIR)/%.o: $(SRC_KERNEL)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(K_OUT_DIR)/%.o: $(SRC_KERNEL)/%.asm
	@mkdir -p $(dir $@)
	@$(NASM) -f elf64 $< -o $@

$(OUT_DIR)/usr/%.o: $(SRC_USER)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -D_FORTIFY_SOURCE=0 -fno-builtin -I$(BIN_LIB_DIR)/targ-include -c $< -o $@

$(APP_OUT_DIR)/%.o: $(SRC_APPS)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -D_FORTIFY_SOURCE=0 -fno-builtin -I$(BIN_LIB_DIR)/targ-include -c $< -o $@


$(APP_OUT_DIR)/syscall.o: $(SRC_USER)/syscall.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -D_FORTIFY_SOURCE=0 -fno-builtin -I$(BIN_LIB_DIR)/targ-include -c $< -o $@

$(APP_OUT_DIR)/crt.o: $(CRT_SRC)
	@mkdir -p $(dir $@)
	@if [ "$(suffix $(CRT_SRC))" = ".asm" ]; then \
		$(NASM) -f elf64 $(CRT_SRC) -o $@; \
	else \
		$(CC) $(CFLAGS) -c $(CRT_SRC) -o $@; \
	fi

$(APP_OUT_DIR)/stdio.o: $(SRC_USER)/stdio_stub.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -D_FORTIFY_SOURCE=0 -fno-builtin -I$(BIN_LIB_DIR)/targ-include -c $< -o $@


user: lib $(ALL_USER_ELFS) $(APP_ELFS)
	@echo "Built user ELFs: $(ALL_USER_ELFS)"

$(APP_OUT_DIR)/%.elf: $(APP_OUT_DIR)/%.o $(APP_OUT_DIR)/syscall.o $(APP_OUT_DIR)/crt.o $(APP_OUT_DIR)/stdio.o
	@mkdir -p $(dir $@)
	@echo "Linking app ELF: $@"
	@if [ -f "$(BIN_LIB_DIR)/libc.a" ]; then \
		$(CC) -nostdlib -static $^ $(USER_LDFLAGS) -o $@; \
	else \
		$(CC) -nostdlib -static $^ -o $@; \
	fi

$(OUT_DIR)/usr/hello.elf: $(OUT_DIR)/usr/hello.o $(OUT_DIR)/usr/syscall.o
	@mkdir -p $(dir $@)
	@echo "Linking user ELF (hello, no -lc): $@"
	@$(CC) -nostdlib -static $^ -o $@

$(OUT_DIR)/usr/malloc_printf_test.elf: $(OUT_DIR)/usr/malloc_printf_test.o $(OUT_DIR)/usr/syscall.o
	@mkdir -p $(dir $@)
	@echo "Linking user ELF: $@"
	@$(CC) -nostdlib -static $^ $(BIN_LIB_DIR)/libc.a $(BIN_LIB_DIR)/libg.a -o $@

lib:
	@mkdir -p $(OUT_DIR)
	@if [ -e "$(BIN_LIB_DIR)" ]; then \
		echo "$(BIN_LIB_DIR) already exists"; \
	else \
		if [ -d "$(SRC_LIB)" ]; then \
			ln -sfn ../$(SRC_LIB) $(BIN_LIB_DIR); \
			echo "Created symlink $(BIN_LIB_DIR) -> $(SRC_LIB)"; \
		else \
			mkdir -p $(BIN_LIB_DIR); \
			echo "Created empty $(BIN_LIB_DIR), please place built lib files here"; \
		fi; \
	fi


$(F_OUT_DIR)/ter-u12b.bdf: $(FONTS)
	@mkdir -p $(F_OUT_DIR)
	@cp $< $@

$(README):
	@mkdir -p $(OUT_DIR)
	@cp README.md $(README)

$(ESP_IMG): $(BOOTX64) $(KERNEL)
	@rm -f $(ESP_IMG)
	@echo "Creating UEFI ESP image..."
	@rm -f $(ESP_IMG)
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

$(EXT2_IMG): $(KERNEL) user
	@rm -f $(EXT2_IMG)
	@echo "Creating FAT16 filesystem image..."
	@rm -rf bin/fs_tmp || true
	@mkdir -p bin/fs_tmp/kernel/fonts
	@mkdir -p bin/fs_tmp/usr
	@mkdir -p bin/fs_tmp/apps
	@mkdir -p bin/fs_tmp/lib
	@cp -f $(FONTS) bin/fs_tmp/kernel/fonts/ 2>/dev/null || true
	@mkdir -p bin/fs_tmp/usr
	@rm -f bin/fs_tmp/usr/*.elf 2>/dev/null || true
	@rm -f bin/fs_tmp/apps/*.elf 2>/dev/null || true
	@for f in $(USER_ELFS); do \
		if [ -f "$$f" ]; then cp -f "$$f" bin/fs_tmp/usr/; fi; \
	done || true
	@for f in $(APP_ELFS); do \
		if [ -f "$$f" ]; then cp -f "$$f" bin/fs_tmp/apps/; fi; \
	done || true
	@cp -f bin/lib/* bin/fs_tmp/lib/ 2>/dev/null || true
	@find bin -type f \
		-not -name "*.o" \
		-not -name "fs.img" \
		-not -path "bin/fs_tmp/*" \
		-not -path "bin/usr/*" \
		-not -path "bin/user/*" \
		-exec bash -c 'dest="bin/fs_tmp/$${1#bin/}"; mkdir -p "$$(dirname "$$dest")"; cp "$$1" "$$dest"' _ {} \;
	@mkdir -p bin/fs_tmp
	@cp README.md bin/fs_tmp/README.md 2>/dev/null || true
	@python3 tools/mk_fat16_image.py $(EXT2_IMG) 256000 bin/fs_tmp
	@rm -rf bin/fs_tmp/fs_content
	@echo "fat16 image created: $(EXT2_IMG)"

run: $(ESP_IMG) $(EXT2_IMG)
	$(QEMU) $(QEMU_FLAGS) --drive file=$(ESP_IMG),format=raw -drive file=$(EXT2_IMG),format=raw,if=ide

run-console: $(ESP_IMG) $(EXT2_IMG)
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd $(CONSOLE)-drive file=$(ESP_IMG),format=raw -drive file=$(EXT2_IMG),format=raw,if=ide

run-vga: $(ESP_IMG) $(EXT2_IMG)
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd $(QEMU_VGA) -drive file=$(ESP_IMG),format=raw -drive file=$(EXT2_IMG),format=raw,if=ide -d int -D qemu.log --no-reboot -monitor stdio

clean-all:
	rm -rf $(OUT_DIR) $(ESP_DIR)
	cd $(EDK2_DIR) && rm -rf bin/boot
	rm -f $(EXT2_IMG)

clean: 
	rm -rf $(OUT_DIR)