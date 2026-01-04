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

# 各アプリケーションディレクトリを自動検出
APP_DIRS := $(wildcard *.app)

.PHONY: all clean $(APP_DIRS)

all: $(CRT_OBJ) $(NEWLIB_SYSCALLS_OBJ) $(OUT_DIR)/stdio.o $(APP_DIRS)

$(APP_DIRS):
	$(MAKE) -C $@

clean:
	@for dir in $(APP_DIRS); do \
		$(MAKE) -C $$dir clean || true; \
	done
	@rm -rf $(OUT_DIR)
# Ensure CRT and helper objects for apps exist in OUT_DIR
$(OUT_DIR)/crt.o: $(CRT_SRC)
	@mkdir -p $(dir $@)
	@$(NASM) -f elf64 $< -o $@

$(OUT_DIR)/newlib_syscalls.o: $(NEWLIB_SYSCALLS_SRC)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/stdio.o: $(STDIO_SRC)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@
