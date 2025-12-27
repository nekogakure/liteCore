#include <unistd.h>
#include <string.h>

int main() {
	const char *msg1 = "Hello, world!\n";
	const char *msg2 = "This application is liteCore's first app!\n";
	const char *msg3 = "Goodbye! ;)\n";

	write(1, msg1, strlen(msg1));
	write(1, msg2, strlen(msg2));
	write(1, msg3, strlen(msg3));

	return 0;
}
