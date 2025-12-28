#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include "cmds/list.h"

char current_path[256] = ".";

void print_prompt() {
	printf("liteCore@%s $ ", current_path);
	fflush(stdout);
}

int parse_command_line(char *line, char *argv[], int max_args) {
	int argc = 0;
	char *p = line;
	int in_token = 0;
	while (*p && argc < max_args) {
		if (*p == ' ' || *p == '\t') {
			if (in_token) {
				*p = '\0';
				in_token = 0;
			}
		} else {
			if (!in_token) {
				argv[argc++] = p;
				in_token = 1;
			}
		}
		p++;
	}
	return argc;
}

shell_command_t *find_command(const char *name) {
	for (int i = 0; commands[i].name; i++) {
		if (strcmp(commands[i].name, name) == 0) {
			return &commands[i];
		}
	}
	return NULL;
}

int main() {
	char line[MAX_LINE];
	while (1) {
		print_prompt();
		if (!fgets(line, sizeof(line), stdin)) {
			break;
		}
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '\0')
			continue;
		char *argv[MAX_ARGS];
		char linebuf[MAX_LINE];
		strncpy(linebuf, line, sizeof(linebuf));
		int argc = parse_command_line(linebuf, argv, MAX_ARGS);
		if (argc == 0)
			continue;
		shell_command_t *cmd = find_command(argv[0]);
		if (cmd) {
			cmd->function(argc, argv);
		} else {
			printf("Unknown command: %s\n", argv[0]);
		}
	}
	return 0;
}
