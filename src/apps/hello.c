#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main() {
	printf("Hello, world!\n");
	printf("This application is LiteCore's first app!\n");

	size_t len = 64;
	char *buf = malloc(len);
	if (!buf) {
		printf("malloc failed\n");
	}

	uintptr_t addr = (uintptr_t)buf;
	snprintf(buf, len, "Allocated %zu bytes at 0x%zx", len, (size_t)addr);
	printf("%s\n", buf);

	size_t newlen = 128;
	char *tmp = realloc(buf, newlen);
	if (!tmp) {
		printf("realloc failed, freeing original buffer\n");
		free(buf);
		return 1;
	}
	buf = tmp;
	size_t used = strlen(buf);
	snprintf(buf + used, newlen - used, " (resized)");
	printf("%s\n", buf);

	free(buf);
	buf = NULL;
	printf("Memory freed\n");

	printf("Goodbye! ;)\n");

	return 0;
}