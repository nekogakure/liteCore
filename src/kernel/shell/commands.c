#include <shell/commands.h>
#include <util/console.h>
#include <stddef.h>

// コマンドの最大数
#define MAX_COMMANDS 128
// 引数の最大数
#define MAX_ARGS 64
// コマンドラインバッファの最大サイズ
#define CMD_BUFFER_SIZE 256

// 登録されたコマンドのリスト
static shell_command_t command_list[MAX_COMMANDS];
static int command_count = 0;

/**
 * @brief 文字列の長さを取得
 */
static size_t str_len(const char *str) {
	size_t len = 0;
	while (str[len])
		len++;
	return len;
}

/**
 * @brief 文字列を比較
 */
static int str_cmp(const char *s1, const char *s2) {
	while (*s1 && *s2 && *s1 == *s2) {
		s1++;
		s2++;
	}
	return *s1 - *s2;
}

/**
 * @brief 文字列をコピー
 */
static void str_cpy(char *dest, const char *src) {
	while (*src) {
		*dest++ = *src++;
	}
	*dest = '\0';
}

/**
 * @brief 文字が空白かどうかを判定
 */
static int is_space(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/**
 * @brief コマンドラインをトークンに分割
 * @param line コマンドライン文字列
 * @param argv 引数配列（出力）
 * @param max_args 最大引数数
 * @return 引数の数
 */
static int parse_command_line(char *line, char *argv[], int max_args) {
	int argc = 0;
	char *p = line;
	int in_token = 0;

	while (*p && argc < max_args) {
		if (is_space(*p)) {
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

/**
 * @brief コマンドを登録
 * @param name コマンド名
 * @param description コマンドの説明
 * @param function コマンドの実行関数
 * @return 成功時0、失敗時-1
 */
int register_command(const char *name, const char *description,
		     command_func_t function) {
	if (command_count >= MAX_COMMANDS) {
		printk("Error: Command list is full\n");
		return -1;
	}

	if (!name || !function) {
		printk("Error: Invalid command registration\n");
		return -1;
	}

	command_list[command_count].name = name;
	command_list[command_count].description = description;
	command_list[command_count].function = function;
	command_count++;

	return 0;
}

/**
 * @brief コマンド名でコマンドを検索
 * @param name コマンド名
 * @return コマンドへのポインタ、見つからない場合NULL
 */
static shell_command_t *find_command(const char *name) {
	for (int i = 0; i < command_count; i++) {
		if (str_cmp(command_list[i].name, name) == 0) {
			return &command_list[i];
		}
	}
	return NULL;
}

/**
 * @brief コマンドを実行
 * @param line コマンドライン文字列
 * @return コマンドの実行結果
 */
int execute_command(char *line) {
	char *argv[MAX_ARGS];
	char buffer[CMD_BUFFER_SIZE];

	// 空行の場合は何もしない
	if (!line || !*line) {
		return 0;
	}

	// 末尾の空白を削除
	char *end = line + str_len(line) - 1;
	while (end >= line && is_space(*end)) {
		*end = '\0';
		end--;
	}

	// 先頭の空白をスキップ
	while (*line && is_space(*line)) {
		line++;
	}

	// 空行チェック
	if (!*line) {
		return 0;
	}

	// コマンドラインをコピー（元の文字列を保護）
	if (str_len(line) >= CMD_BUFFER_SIZE) {
		printk("Error: Command line too long\n");
		return -1;
	}
	str_cpy(buffer, line);

	// コマンドラインをパース
	int argc = parse_command_line(buffer, argv, MAX_ARGS);
	if (argc == 0) {
		return 0;
	}

	// コマンドを検索
	shell_command_t *cmd = find_command(argv[0]);
	if (!cmd) {
		printk("Error: Unknown command '%s'\n", argv[0]);
		return -1;
	}

	// コマンドを実行
	return cmd->function(argc, argv);
}

/**
 * @brief 登録されている全コマンドを表示
 */
void list_commands(void) {
	printk("Available commands:\n");
	for (int i = 0; i < command_count; i++) {
		printk("  %-12s - %s\n", command_list[i].name,
		       command_list[i].description ?
			       command_list[i].description :
			       "No description");
	}
}

/**
 * @brief コマンドシステムの初期化
 */
void init_commands(void) {
	command_count = 0;
}

/**
 * @brief helpコマンド - 利用可能なコマンドを表示
 */
static int cmd_help(int argc, char **argv) {
	(void)argc;
	(void)argv;
	list_commands();
	return 0;
}

/**
 * @brief echoコマンド - 引数を表示
 */
static int cmd_echo(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		printk("%s", argv[i]);
		if (i < argc - 1) {
			printk(" ");
		}
	}
	printk("\n");
	return 0;
}

/**
 * @brief clearコマンド - 画面をクリア（簡易版）
 */
static int cmd_clear(int argc, char **argv) {
	(void)argc;
	(void)argv;
	// 簡易実装: 複数の改行で画面をスクロール
	for (int i = 0; i < 25; i++) {
		printk("\n");
	}
	printk("\033[2J\033[H"); // ANSI escape（サポートされている場合）
	return 0;
}

/**
 * @brief 組み込みコマンドを登録
 */
void register_builtin_commands(void) {
	register_command("help", "Display available commands", cmd_help);
	register_command("echo", "Echo arguments to console", cmd_echo);
	register_command("clear", "Clear the console screen", cmd_clear);
}
