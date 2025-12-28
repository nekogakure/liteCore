CC      = gcc
NASM    = nasm

APPNAME = shell
OUT_DIR = ../../bin/apps
USER_DIR = ../user
LIB_DIR = ../../bin/lib
NEWLIB_DIR = ../../newlib/newlib/libc/include

CFLAGS  = -O2 -I$(NEWLIB_DIR) -I$(USER_DIR) -I. -Wall
LDFLAGS = -nostdlib -static -no-pie -L$(LIB_DIR) -lc

CRT_OBJ = $(OUT_DIR)/crt.o
NEWLIB_SYSCALLS_OBJ = $(OUT_DIR)/newlib_syscalls.o
STDIO_OBJ = $(OUT_DIR)/stdio.o

ENTRY_SRC := $(shell grep -E '^[^#].+\\.c' .config | head -n1)
ifeq ($(ENTRY_SRC),)
$(error please specify an entry source file in .config)
endif

OBJ = $(ENTRY_SRC:.c=.o)
TARGET  = $(OUT_DIR)/$(APPNAME).elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ) $(CRT_OBJ) $(NEWLIB_SYSCALLS_OBJ) $(STDIO_OBJ) manifest.txt
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(CRT_OBJ) $(NEWLIB_SYSCALLS_OBJ) $(STDIO_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(TARGET)