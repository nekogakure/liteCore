#!/bin/bash

set -e

APPS_DIR="$(dirname "$0")/../src/apps"

echo "$ENTRY" > .config
echo "# $APPNAME app manifest" > manifest.txt

read -p "app name: " APPNAME
APPDIR="$APPS_DIR/${APPNAME}.app"

if [ -d "$APPDIR" ]; then
	echo "[error] $APPDIR already exists."
	exit 1
fi

mkdir -p "$APPDIR"
mkdir -p "$APPDIR/src"
mkdir -p "$APPDIR/res"
cd "$APPDIR"


read -p "version: " VERSION
if [ -z "$VERSION" ]; then VERSION="1.0"; fi
read -p "author (example: nekogakure<info@mail.nekogakure.jp>): " AUTHOR
if [ -z "$AUTHOR" ]; then AUTHOR="unknown"; fi
read -p "description: " DESC
if [ -z "$DESC" ]; then DESC="No description."; fi
read -p "icon path (example: res/icon.png, optional): " ICON
if [ -z "$ICON" ]; then ICON="res/icon.png"; fi

# Entry point
read -p "entry point C file name (example: src/main.c): " ENTRY
if [ -z "$ENTRY" ]; then
	ENTRY="src/main.c"
fi

echo "$ENTRY" > .config

echo "$APPNAME" > manifest.txt
echo "$VERSION" >> manifest.txt
echo "$AUTHOR" >> manifest.txt
echo "$DESC" >> manifest.txt
echo "$ICON" >> manifest.txt

touch "$ENTRY"

# Makefile template
cat > Makefile << 'EOF'
CC      = gcc
NASM    = nasm

APPNAME = $(notdir $(CURDIR:.app=))
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
EOF

echo "[OK] $APPDIR has been created."
echo "Entry point: $ENTRY"
echo "manifest.txt, .config, Makefile have also been generated."