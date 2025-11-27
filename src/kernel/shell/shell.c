#include <shell/shell.h>
#include <shell/commands.h>
#include <shell/commands.h>
#include <util/console.h>
#include <device/keyboard.h>

#define SHELL_BUFFER_SIZE 256

static char input_buffer[SHELL_BUFFER_SIZE];
static int buffer_pos = 0;

/**
 * @brief シェルプロンプトを表示
 */
static void show_prompt(void) {
	const char *cwd = get_current_directory();

	/*
	// パスから最後のディレクトリ名を抽出
	const char *dir_name = cwd;
	for (int i = 0; cwd[i] != '\0'; i++) {
		if (cwd[i] == '/' && cwd[i + 1] != '\0') {
			dir_name = &cwd[i + 1];
		}
	}
	*/

	// ルートディレクトリの場合
	if (cwd[0] == '/' && cwd[1] == '\0') {
		printk("LiteCore@/ $ ");
	} else {
		printk("LiteCore@%s $ ", cwd);
	}
}

/**
 * @brief バッファをクリア
 */
static void clear_buffer(void) {
	buffer_pos = 0;
	input_buffer[0] = '\0';
}

/**
 * @brief シェルの初期化
 */
void init_shell(void) {
	printk("\n");
	printk("========================================\n");
	printk("       Welcome to LiteCore Shell!       \n");
	printk("   Type 'help' for available commands   \n");
	printk("========================================\n");
	printk("\n");

	// コマンドシステムの初期化
	init_commands();
	register_builtin_commands();

	clear_buffer();
	show_prompt();
}

/**
 * @brief 1文字を処理
 * @param c 入力文字
 * @return コマンドが実行された場合1、そうでなければ0
 */
static int process_char(char c) {
	// Enter (改行)
	if (c == '\n' || c == '\r') {
		printk("\n");

		if (buffer_pos > 0) {
			input_buffer[buffer_pos] = '\0';
			execute_command(input_buffer);
		}

		clear_buffer();
		show_prompt();
		return 1;
	}

	// Backspace (0x08 or 0x7F/127)
	if (c == '\b' || c == 0x08 || c == 127 || c == 0x7F) {
		if (buffer_pos > 0) {
			buffer_pos--;
			input_buffer[buffer_pos] = '\0';
			// カーソルを1文字戻して空白で上書きしてまた戻す
			printk("\b \b");
		}
		return 0;
	}

	// Tab - 無視（将来的に補完機能用）
	if (c == '\t') {
		return 0;
	}

	// ESC - 無視
	if (c == 27) {
		return 0;
	}

	// 通常の表示可能文字
	if (c >= 32 && c < 127) {
		if (buffer_pos < SHELL_BUFFER_SIZE - 1) {
			input_buffer[buffer_pos++] = c;
			input_buffer[buffer_pos] = '\0';
			printk("%c", c);
		}
		return 0;
	}

	// その他の制御文字は無視
	return 0;
}

/**
 * @brief 1行のコマンドを読み取って実行（ポーリング版）
 * @return 処理した文字数
 */
int shell_readline_and_execute(void) {
	// キーボードから文字を取得（ポーリング）
	char c = keyboard_getchar_poll();

	if (c != 0) {
		return process_char(c);
	}

	return 0;
}

/**
 * @brief シェルのメインループ（ブロッキング版）
 */
void shell_run(void) {
	char c = keyboard_getchar();
	process_char(c);
}