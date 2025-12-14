# User Programs Makefile
# Build user ELF executables and coordinate apps build

CC         = gcc

# Directories
SRC_DIR    = .
OUT_DIR    = ../../bin/usr
APP_DIR    = ../apps
LIB_DIR    = ../../bin/lib

# Compiler flags for user programs
CFLAGS     = -O2 -Wimplicit-function-declaration -Wunused-but-set-variable \
             -ffreestanding -m64 -c -Wall -Wextra \
             -mcmodel=large -mno-red-zone -fno-pic \
             -fcf-protection=none -D_FORTIFY_SOURCE=0 -fno-builtin \
             -I$(LIB_DIR)/targ-include

# Linker flags
LDFLAGS    = -nostdlib -static

# Source files (only in src/user, excluding helper files)
USER_SOURCES = $(shell find $(SRC_DIR) -maxdepth 1 -name "*.c" \! -name "syscall.c" \! -name "stdio_stub.c")
USER_OBJECTS = $(patsubst $(SRC_DIR)/%.c, $(OUT_DIR)/%.o, $(USER_SOURCES))
USER_ELFS    = $(patsubst $(SRC_DIR)/%.c, $(OUT_DIR)/%.elf, $(USER_SOURCES))

.PHONY: all clean lib apps

all: lib $(USER_ELFS) apps
	@echo "Built all user programs"

# Link to library directory
lib:
	@mkdir -p ../../bin
	@if [ -e "$(LIB_DIR)" ]; then \
		echo "$(LIB_DIR) already exists"; \
	else \
		if [ -d "../../src/lib" ]; then \
			ln -sfn ../src/lib $(LIB_DIR); \
			echo "Created symlink $(LIB_DIR) -> ../../src/lib"; \
		else \
			mkdir -p $(LIB_DIR); \
			echo "Created empty $(LIB_DIR), please place built lib files here"; \
		fi; \
	fi

# Build apps directory
apps:
	@if [ -d "$(APP_DIR)" ]; then \
		echo "==> Building apps..."; \
		$(MAKE) -C $(APP_DIR); \
	else \
		echo "No apps directory found, skipping"; \
	fi

# User program object files (if any exist in src/user)
$(OUT_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Link user ELFs (if any exist in src/user)
$(OUT_DIR)/%.elf: $(OUT_DIR)/%.o $(OUT_DIR)/syscall.o
	@mkdir -p $(dir $@)
	@echo "Linking user ELF: $@"
	@$(CC) $(LDFLAGS) $^ -o $@

# Syscall object (for src/user programs only)
$(OUT_DIR)/syscall.o: $(SRC_DIR)/syscall.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OUT_DIR)
	@if [ -d "$(APP_DIR)" ]; then \
		$(MAKE) -C $(APP_DIR) clean; \
	fi
