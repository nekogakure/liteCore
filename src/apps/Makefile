CC         = gcc
NASM       = nasm

# Directories
SRC_DIR    = .
OUT_DIR    = ../../bin/apps
USER_DIR   = ../user
LIB_DIR    = ../../bin/lib
NEWLIB_DIR = ../../newlib/newlib/libc/include

CFLAGS     = -O2 -Wimplicit-function-declaration -Wunused-but-set-variable \
			 -ffreestanding -m64 -c -Wall -Wextra \
			 -mcmodel=small -mno-red-zone -fno-pic -no-pie \
			 -fcf-protection=none -D_FORTIFY_SOURCE=0 -fno-builtin \
			 -D__litecore__ \
			 -I$(LIB_DIR)/targ-include -I$(NEWLIB_DIR)

# Linker flags
LDFLAGS    = -nostdlib -static -no-pie
USER_LDFLAGS = -L$(LIB_DIR) -lc

# Source files - automatically detect all .c files (exclude linux_syscalls.c)
APP_SOURCES = $(filter-out $(SRC_DIR)/linux_syscalls.c, $(wildcard $(SRC_DIR)/*.c))
APP_OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(OUT_DIR)/%.o, $(APP_SOURCES))
APP_ELFS    = $(patsubst $(SRC_DIR)/%.c, $(OUT_DIR)/%.elf, $(APP_SOURCES))

# CRT (C runtime startup)
CRT_SRC = $(USER_DIR)/crt.asm
CRT_OBJ = $(OUT_DIR)/crt.o

# System call wrapper
SYSCALL_SRC = $(USER_DIR)/syscall.c
SYSCALL_OBJ = $(OUT_DIR)/syscall.o

# Syscalls implementation (Linux-compatible, works on both Linux and LiteCore)
NEWLIB_SYSCALLS_SRC = $(USER_DIR)/linux_syscalls.c
NEWLIB_SYSCALLS_OBJ = $(OUT_DIR)/newlib_syscalls.o

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

# Newlib syscalls object
$(NEWLIB_SYSCALLS_OBJ): $(NEWLIB_SYSCALLS_SRC)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Stdio stub object
$(STDIO_OBJ): $(STDIO_SRC)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Link app ELFs with newlib
$(OUT_DIR)/%.elf: $(OUT_DIR)/%.o $(CRT_OBJ) $(NEWLIB_SYSCALLS_OBJ) $(STDIO_OBJ)
	@mkdir -p $(dir $@)
	@echo "Linking app ELF: $@"
	@if [ -f "$(LIB_DIR)/libc.a" ]; then \
		$(CC) $(LDFLAGS) -Wl,--build-id=none $^ $(USER_LDFLAGS) -o $@; \
	else \
		echo "Warning: libc.a not found, linking without newlib"; \
		$(CC) $(LDFLAGS) -Wl,--build-id=none $^ -o $@; \
	fi

# Special case: test.elf (minimal, no libc)
$(OUT_DIR)/test.elf: $(OUT_DIR)/test.o
	@mkdir -p $(dir $@)
	@echo "Linking minimal test ELF: $@"
	@$(CC) -nostdlib -static -no-pie -Wl,--build-id=none $< -o $@

# Special case: hello_simple.elf (minimal, no libc, direct syscalls)
$(OUT_DIR)/hello_simple.elf: $(OUT_DIR)/hello_simple.o
	@mkdir -p $(dir $@)
	@echo "Linking minimal hello_simple ELF: $@"
	@$(CC) -nostdlib -static -no-pie -Wl,--build-id=none $< -o $@

# Linux-compatible build target (now default)
linux-compat: $(OUT_DIR)/hello-linux
	@echo "Linux-compatible binaries are now built by default (all target)"
	@echo "This target creates a simplified Linux-only build for testing"

$(OUT_DIR)/hello-linux: $(SRC_DIR)/hello.c
	@mkdir -p $(dir $@)
	@echo "Building simplified Linux-only hello"
	@$(CC) -O2 -m64 -Wall -Wextra $(SRC_DIR)/hello.c -o $@
	@echo "Created: $@ (Linux-only, uses glibc)"

clean:
	rm -rf $(OUT_DIR)
