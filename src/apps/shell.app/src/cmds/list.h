#ifndef APP_SHELL_CMDS_LIST_H
#define APP_SHELL_CMDS_LIST_H

#include <stdio.h>

#define MAX_LINE 256
#define MAX_ARGS 64

int cmd_help(int argc, char **argv);
int cmd_echo(int argc, char **argv);
int cmd_clear(int argc, char **argv);
int cmd_info(int argc, char **argv);
int cmd_exit(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_ls(int argc, char **argv);
int cmd_cat(int argc, char **argv);

typedef int (*command_func_t)(int argc, char **argv);

typedef struct {
	const char *name;
	const char *description;
	command_func_t function;
} shell_command_t;

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

#endif // APP_SHELL_CMDS_LIST_H