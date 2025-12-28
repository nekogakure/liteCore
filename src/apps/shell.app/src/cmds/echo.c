#include <stdio.h>
#include <string.h>
#include "list.h"

int cmd_echo(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		printf("%s", argv[i]);
		if (i < argc - 1)
			printf(" ");
	}
	printf("\n");
	return 0;
}
