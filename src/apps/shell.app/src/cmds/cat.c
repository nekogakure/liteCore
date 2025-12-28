#include <stdio.h>
#include <string.h>
#include "list.h"

int cmd_cat(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage: cat <filename>\n");
		return -1;
	}
	FILE *fp = fopen(argv[1], "r");
	if (!fp) {
		printf("cat: cannot open %s\n", argv[1]);
		return -1;
	}
	char buf[256];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		fwrite(buf, 1, n, stdout);
	}
	fclose(fp);
	return 0;
}
