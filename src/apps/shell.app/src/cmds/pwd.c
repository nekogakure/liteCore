#include <stdio.h>
#include <string.h>
#include "list.h"

extern char current_path[256];

int cmd_pwd(int argc, char **argv) {
	(void)argc;
	(void)argv;
	printf("%s\n", current_path);
	return 0;
}
