#include <stdio.h>
#include "list.h"

int cmd_clear(int argc, char **argv) {
	(void)argc;
	(void)argv;
	printf("\033[2J\033[H");
	return 0;
}
