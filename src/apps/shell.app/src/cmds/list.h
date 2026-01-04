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
int listdir(const char *path);

typedef int (*command_func_t)(int argc, char **argv);

typedef struct {
	const char *name;
	const char *description;
	command_func_t function;
} shell_command_t;

extern shell_command_t commands[];

#endif // APP_SHELL_CMDS_LIST_H