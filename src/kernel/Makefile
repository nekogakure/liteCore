CC         = gcc
LD         = ld
NASM       = nasm
OBJCOPY    = objcopy

# Directories
SRC_DIR    = .
OUT_DIR    = ../../bin/kernel
FONTS_SRC  = fonts/ter-u12b.bdf
FONTS_OUT  = $(OUT_DIR)/fonts

# Compiler/Linker flags
CFLAGS     = -O2 -Wimplicit-function-declaration -Wunused-but-set-variable \
             -ffreestanding -m64 -c -Wall -Wextra -I. \
             -mcmodel=large -mno-red-zone -fno-pic
LDFLAGS    = -m elf_x86_64 -z max-page-size=0x1000
LINKER     = ../kernel.ld

# Source files
SOURCES     = $(shell find . -name "*.c")
ASM_SOURCES = $(shell find . -name "*.asm")
OBJECTS     = $(shell printf "%s\n" \
	$(patsubst ./%.c, $(OUT_DIR)/%.o, $(SOURCES)) \
	$(patsubst ./%.asm, $(OUT_DIR)/%.o, $(ASM_SOURCES)) | sort -u)

# Targets
KERNEL_ELF = $(OUT_DIR)/kernel.elf
KERNEL_BIN = $(OUT_DIR)/kernel.bin

.PHONY: all clean

all: $(KERNEL_BIN) $(FONTS_OUT)/ter-u12b.bdf

$(OUT_DIR):
	@mkdir -p $(OUT_DIR)

$(KERNEL_ELF): $(OUT_DIR) $(OBJECTS) $(LINKER)
	$(LD) $(LDFLAGS) -T $(LINKER) $(OBJECTS) -o $@

$(KERNEL_BIN): $(KERNEL_ELF)
	@$(OBJCOPY) -O binary $< $@
	@echo "Kernel size: $$(wc -c < $@) bytes"

$(OUT_DIR)/%.o: ./%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/%.o: ./%.asm
	@mkdir -p $(dir $@)
	@$(NASM) -f elf64 $< -o $@

$(FONTS_OUT)/ter-u12b.bdf: $(FONTS_SRC)
	@mkdir -p $(FONTS_OUT)
	@cp $< $@

clean:
	rm -rf $(OUT_DIR)
