#include "list.h"

shell_command_t commands[] = {
	{ "help", "Display available commands", cmd_help },
	{ "echo", "Echo arguments to console", cmd_echo },
	{ "clear", "Clear the console screen", cmd_clear },
	{ "info", "Show app info (manifest.txt)", cmd_info },
	{ "exit", "Exit the shell", cmd_exit },
	{ "pwd", "Print current directory", cmd_pwd },
	{ "cd", "Change directory", cmd_cd },
	{ "ls", "List files", cmd_ls },
	{ "cat", "Show file contents", cmd_cat },
	{ NULL, NULL, NULL }
};
