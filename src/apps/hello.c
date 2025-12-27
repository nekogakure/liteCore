#include <stdio.h>

int main() {
	setvbuf(stdout, NULL, _IONBF, 0);

	printf("Hello, world!\n");
	printf("This application is liteCore's first app!\n");
	printf("Goodbye! ;)\n");
	fflush(stdout);

	return 0;
}