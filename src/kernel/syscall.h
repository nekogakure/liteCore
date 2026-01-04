#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>

#define SYS_read 0 /// ファイルから読み込む
#define SYS_write 1 /// ファイルに書き込む
#define SYS_open 2 /// ファイルを開く
#define SYS_close 3 /// ファイルを閉じる
#define SYS_fstat 5 /// ファイルの状態を取得する
#define SYS_lseek 8 /// ファイル位置を変更する
#define SYS_sbrk 12 /// ヒープ領域を拡張する
#define SYS_getpid 39 /// プロセスIDを取得する
#define SYS_exit 60 /// プロセスを終了する
#define SYS_kill 62 /// プロセスにシグナルを送る
#define SYS_isatty 100 /// ファイルディスクリプタが端末か確認する
#define SYS_arch_prctl 158 /// アーキテクチャ固有のプロセス制御
#define SYS_get_reent 200 /// 再入可能構造体を取得する
#define SYS_fork 201 /// プロセスを複製する
#define SYS_execve 202 /// プログラムを実行する
#define SYS_waitpid 203 /// 子プロセスの終了を待つ
#define SYS_mmap 209 /// メモリマッピングを行う
#define SYS_munmap 210 /// メモリマッピングを解除する
#define SYS_mprotect 211 /// メモリ保護属性を変更する
#define SYS_chdir 212 /// カレントディレクトリを変更する
#define SYS_getcwd 213 /// カレントディレクトリを取得する
#define SYS_listdir \
	214 /// ディレクトリの内容を一覧表示する（デバッグ/ユーザ向けラッパー）

void syscall_entry_c(uint64_t *regs_stack, uint32_t vec);
void syscall_init(void);

#endif /* _KERNEL_SYSCALL_H */