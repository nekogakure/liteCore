# Apps Makefile
# Build application ELF executables with newlib

CC         = gcc
NASM       = nasm

# Directories
SRC_DIR    = .
OUT_DIR    = ../../bin/apps
USER_DIR   = ../user
LIB_DIR    = ../../bin/lib

# Compiler flags for user programs
CFLAGS     = -O2 -Wimplicit-function-declaration -Wunused-but-set-variable \
             -ffreestanding -m64 -c -Wall -Wextra \
             -mcmodel=large -mno-red-zone -fno-pic \
             -fcf-protection=none -D_FORTIFY_SOURCE=0 -fno-builtin \
             -I$(LIB_DIR)/targ-include

# Linker flags
LDFLAGS    = -nostdlib -static
USER_LDFLAGS = -L$(LIB_DIR) -lc

# Source files - automatically detect all .c files
APP_SOURCES = $(wildcard $(SRC_DIR)/*.c)
APP_OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(OUT_DIR)/%.o, $(APP_SOURCES))
APP_ELFS    = $(patsubst $(SRC_DIR)/%.c, $(OUT_DIR)/%.elf, $(APP_SOURCES))

# CRT (C runtime startup)
CRT_SRC = $(USER_DIR)/crt.asm
CRT_OBJ = $(OUT_DIR)/crt.o

# System call wrapper
SYSCALL_SRC = $(USER_DIR)/syscall.c
SYSCALL_OBJ = $(OUT_DIR)/syscall.o

# Stdio stub
STDIO_SRC = $(USER_DIR)/stdio_stub.c
STDIO_OBJ = $(OUT_DIR)/stdio.o

.PHONY: all clean

all: $(APP_ELFS)
	@echo "Built app ELFs: $(APP_ELFS)"

# App object files
$(OUT_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# CRT object
$(CRT_OBJ): $(CRT_SRC)
	@mkdir -p $(dir $@)
	@$(NASM) -f elf64 $(CRT_SRC) -o $@

# Syscall object
$(SYSCALL_OBJ): $(SYSCALL_SRC)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Stdio stub object
$(STDIO_OBJ): $(STDIO_SRC)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Link app ELFs with newlib
$(OUT_DIR)/%.elf: $(OUT_DIR)/%.o $(CRT_OBJ) $(SYSCALL_OBJ) $(STDIO_OBJ)
	@mkdir -p $(dir $@)
	@echo "Linking app ELF: $@"
	@if [ -f "$(LIB_DIR)/libc.a" ]; then \
		$(CC) $(LDFLAGS) $^ $(USER_LDFLAGS) -o $@; \
	else \
		echo "Warning: libc.a not found, linking without newlib"; \
		$(CC) $(LDFLAGS) $^ -o $@; \
	fi

# Special case: test.elf (minimal, no libc)
$(OUT_DIR)/test.elf: $(OUT_DIR)/test.o
	@mkdir -p $(dir $@)
	@echo "Linking minimal test ELF: $@"
	@$(CC) -nostdlib -static -no-pie -Wl,--build-id=none $< -o $@

clean:
	rm -rf $(OUT_DIR)
