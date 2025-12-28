#include <stdio.h>
#include <string.h>
#include "../main.c"
#include "list.h"

extern shell_command_t commands[];

int cmd_help(int argc, char **argv) {
	(void)argc;
	(void)argv;
	printf("Available commands:\n");
	for (int i = 0; commands[i].name; i++) {
		printf("  %-8s - %s\n", commands[i].name,
		       commands[i].description);
	}
	return 0;
}
