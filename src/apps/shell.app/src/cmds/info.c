#include <stdio.h>
#include <string.h>
#include "list.h"

int cmd_info(int argc, char **argv) {
	(void)argc;
	(void)argv;
	FILE *fp = fopen("manifest.txt", "r");
	if (fp) {
		char buf[128];
		int i = 0;
		const char *fields[] = { "Name", "Version", "Author",
					 "Description", "Icon" };
		while (fgets(buf, sizeof(buf), fp) && i < 5) {
			buf[strcspn(buf, "\r\n")] = 0;
			printf("%s: %s\n", fields[i], buf);
			i++;
		}
		fclose(fp);
	} else {
		printf("manifest.txt not found\n");
	}
	return 0;
}
