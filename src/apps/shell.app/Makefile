CC      = gcc
NASM    = nasm

APPNAME = shell
OUT_DIR = ../../../bin/apps
APP_PACKAGE_DIR = $(OUT_DIR)/$(APPNAME).app
USER_DIR = ../user
# LIB_DIR should point to repository root bin/lib
LIB_DIR = ../../../bin/lib
# NEWLIB include dir (repo root relative)
NEWLIB_DIR = ../../../newlib/newlib/libc/include

# Compile flags aligned with top-level apps Makefile to avoid fortified
# wrappers and builtin substitutions that require host libc symbols.
CFLAGS  = -O2 -I$(LIB_DIR)/targ-include -I$(NEWLIB_DIR) -I$(USER_DIR) -I. -Wall \
			-D__litecore__ -D_FORTIFY_SOURCE=0 -fno-builtin -fno-stack-protector \
			-ffreestanding -m64 -mcmodel=small -mno-red-zone -fno-pic -no-pie \
			-fcf-protection=none
LDFLAGS = -nostdlib -static -no-pie -L$(LIB_DIR)

CRT_OBJ = $(OUT_DIR)/crt.o
NEWLIB_SYSCALLS_OBJ = $(OUT_DIR)/newlib_syscalls.o
STDIO_OBJ = $(OUT_DIR)/stdio.o

ENTRY_SRC := $(shell awk '/^[ \t]*#/ {next} /^[ \t]*$$/ {next} { if ($$0 ~ /\.c$$/) { print $$1; exit } }' .config)
ifeq ($(ENTRY_SRC),)
$(error please specify an entry source file in .config)
endif

SRC_DIR = src
APP_OUT_DIR = $(OUT_DIR)/$(APPNAME)
SRCS := $(shell find $(SRC_DIR) -name '*.c' -print)
OBJ = $(patsubst $(SRC_DIR)/%.c,$(APP_OUT_DIR)/%.o,$(SRCS))
TARGET  = $(OUT_DIR)/$(APPNAME).elf
RESOURCE_DIR := $(shell sed -n '2p' .config 2>/dev/null | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$$//')

.PHONY: all clean


all: $(TARGET)

$(TARGET): $(OBJ) $(CRT_OBJ) $(NEWLIB_SYSCALLS_OBJ) $(STDIO_OBJ) manifest.txt .config
	@mkdir -p $(OUT_DIR)
	@echo "Linking temporary ELF: $@"
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(CRT_OBJ) $(NEWLIB_SYSCALLS_OBJ) $(STDIO_OBJ) -lc -lg

	@echo "Packaging app into $(APP_PACKAGE_DIR)"
	@rm -rf $(APP_PACKAGE_DIR)
	@mkdir -p $(APP_PACKAGE_DIR)
	@cp -f $@ $(APP_PACKAGE_DIR)/entry.elf
	@cp -f manifest.txt $(APP_PACKAGE_DIR)/manifest.txt

	# Copy resource directory/file listed on second line of .config (if any)
	if [ -n "$(RESOURCE_DIR)" ] ; then \
		if [ -d "$(SRC_DIR)/$(RESOURCE_DIR)" ] ; then \
			cp -r "$(SRC_DIR)/$(RESOURCE_DIR)" "$(APP_PACKAGE_DIR)/" ; \
		elif [ -f "$(SRC_DIR)/$(RESOURCE_DIR)" ] ; then \
			cp -f "$(SRC_DIR)/$(RESOURCE_DIR)" "$(APP_PACKAGE_DIR)/" ; \
		fi ; \
	fi


$(APP_OUT_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning shell app objects"
	@rm -f $(TARGET)
	@find $(APP_OUT_DIR) -name '*.o' -delete || true