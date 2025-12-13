#include <util/config.h>
#include <util/console.h>
#include <util/io.h>
#include <util/bdf.h>
#include <stdarg.h>
#include <stdint.h>
#include <mem/manager.h>
#include <interrupt/irq.h>
#include <boot_info.h>

// GOP フレームバッファ情報
static uint32_t *framebuffer = NULL;

static int gfx_cols = 0;
static int gfx_rows = 0;
static char *gfx_buf = NULL;

static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;
static int use_framebuffer = 0;

static uint32_t fb_fg_color = 0xFFFFFF; // 白
static uint32_t fb_bg_color = 0x000000; // 黒

static void allocate_gfx_buf_if_needed(void) {
	if (!use_framebuffer)
		return;
	if (gfx_buf)
		return;

	const bdf_font_t *font = bdf_get_font();
	int fw = font ? (int)font->width : 8;
	int fh = font ? (int)font->height : 16;
	if (fw <= 0)
		fw = 8;
	if (fh <= 0)
		fh = 16;

	gfx_cols = (int)(fb_width / fw);
	gfx_rows = (int)(fb_height / fh);
	if (gfx_cols <= 0)
		gfx_cols = 1;
	if (gfx_rows <= 0)
		gfx_rows = 1;

	uint32_t size = (uint32_t)(gfx_cols * gfx_rows);
	gfx_buf = (char *)kmalloc(size);
	if (!gfx_buf) {
		gfx_cols = gfx_rows = 0;
		return;
	}
	for (uint32_t i = 0; i < size; i++)
		gfx_buf[i] = ' ';
}

/**
 * @brief GOPフレームバッファ情報を設定
 */
void console_set_framebuffer(BOOT_INFO *boot_info) {
	if (boot_info && boot_info->FramebufferBase != 0) {
		framebuffer = (uint32_t *)(uintptr_t)boot_info->FramebufferBase;
		fb_width = boot_info->HorizontalResolution;
		fb_height = boot_info->VerticalResolution;
		fb_pitch = boot_info->PixelsPerScanLine;
		use_framebuffer = 1;
	} else {
		use_framebuffer = 0;
	}
}

/**
 * @brief フレームバッファの色を設定
 * @param fg 前景色
 * @param bg 背景色
 */
void console_set_colors(uint32_t fg, uint32_t bg) {
	fb_fg_color = fg;
	fb_bg_color = bg;
}

/**
 * @brief フレームバッファの色を取得
 * @param fg 前景色を格納するポインタ
 * @param bg 背景色を格納するポインタ
 */
void console_get_colors(uint32_t *fg, uint32_t *bg) {
	if (fg) {
		*fg = fb_fg_color;
	}
	if (bg) {
		*bg = fb_bg_color;
	}
}

/**
 * @brief フレームバッファに文字を描画
 */
static void draw_char_fb(int x, int y, char c) {
	if (!use_framebuffer) {
		return;
	}

	const bdf_font_t *font_info = bdf_get_font();
	if (!font_info) {
		return;
	}

	const bdf_glyph_t *glyph = bdf_get_glyph((uint32_t)(unsigned char)c);

	int char_width = (glyph && glyph->width) ? (int)glyph->width :
						   (int)font_info->width;
	int char_height = (glyph && glyph->height) ? (int)glyph->height :
						     (int)font_info->height;

	if (!glyph || c == ' ') {
		for (int row = 0; row < char_height; row++) {
			for (int col = 0; col < char_width; col++) {
				uint32_t pixel_x = x * char_width + col;
				uint32_t pixel_y = y * char_height + row;
				if (pixel_x < fb_width && pixel_y < fb_height) {
					uint32_t offset =
						pixel_y * fb_pitch + pixel_x;
					framebuffer[offset] = fb_bg_color;
				}
			}
		}
		return;
	}

	for (int row = 0; row < char_height && row < (int)MAX_GLYPH_HEIGHT;
	     row++) {
		uint16_t bits = glyph->bitmap[row];
		for (int col = 0; col < char_width; col++) {
			uint32_t pixel_x = x * char_width + col;
			uint32_t pixel_y = y * char_height + row;

			if (pixel_x < fb_width && pixel_y < fb_height) {
				uint32_t offset = pixel_y * fb_pitch + pixel_x;
				int bitpos = (char_width - 1) - col;
				uint16_t mask = (bitpos >= 0 && bitpos < 16) ?
							(1u << bitpos) :
							0;
				framebuffer[offset] = (bits & mask) ?
							      fb_fg_color :
							      fb_bg_color;
			}
		}
	}
}

/**
 * @brief シリアルポート（COM1）を初期化
 */
static void serial_init(void) {
	outb(0x3f8 + 1, 0x00); // すべての割り込みを無効化
	outb(0x3f8 + 3, 0x80); // DLABを有効化
	outb(0x3f8 + 0, 0x03); // 分周値下位バイト (38400ボーレート)
	outb(0x3f8 + 1, 0x00); // 分周値上位バイト
	outb(0x3f8 + 3, 0x03); // 8ビット、パリティなし、ストップビット1
	outb(0x3f8 + 2, 0xC7); // FIFO有効化
	outb(0x3f8 + 4, 0x0B); // IRQ有効化、RTS/DSRセット
}

/**
 * @brief シリアルポートに1文字出力
 */
static void serial_putc(char c) {
	while ((inb(0x3f8 + 5) & 0x20) == 0) {
	}
	outb(0x3f8, (uint8_t)c);
}

/**
 * @brief シリアルポートからデータが利用可能かチェック
 */
int serial_received(void) {
	return inb(0x3f8 + 5) & 1;
}

/**
 * @brief シリアルポートから1文字読み取る
 */
char serial_getc(void) {
	while (serial_received() == 0) {
	}
	return inb(0x3f8);
}

/**
 * @brief シリアルポートから1文字読み取る（ノンブロッキング）
 * @return 文字、またはデータがない場合は0
 */
char serial_getc_nonblock(void) {
	if (serial_received()) {
		return inb(0x3f8);
	}
	return 0;
}

/** 
 * @var cursor_row
 * @brief コンソール内のカーソルの現在の行位置
 */
static int cursor_row = 0;

/**
 * @var cursor_col
 * @brief コンソール内のカーソルの現在の列位置
 */
static int cursor_col = 0;

static const int CONSOLE_COLS = 80;

/**
 * @var CONSOLE_ROWS
 * @brief コンソール行数
 */
static const int CONSOLE_ROWS = 25;

/**
 * @def N_HISTORY
 * @brief コンソールの最大保存容量
 */
#define N_HISTORY 100
static char history[N_HISTORY][80];
static int history_lines = 0;
static int history_offset = 0;

void console_init() {
	serial_init(); // シリアルポートを初期化

	allocate_gfx_buf_if_needed();

	if (gfx_buf && gfx_cols > 0 && gfx_rows > 0) {
		uint32_t size = (uint32_t)(gfx_cols * gfx_rows);
		for (uint32_t i = 0; i < size; ++i)
			gfx_buf[i] = ' ';
	}

	if (use_framebuffer && framebuffer) {
		uint32_t color = fb_bg_color;
		for (uint32_t y = 0; y < fb_height; ++y) {
			for (uint32_t x = 0; x < fb_width; ++x) {
				framebuffer[y * fb_pitch + x] = color;
			}
		}
	}

	cursor_row = 0;
	cursor_col = 0;
	history_lines = 0;
	history_offset = 0;
}

/**
 * @fn new_line
 * @brief 改行を行い、スクロールが必要ならスクロールする
 */
void new_line() {
	cursor_col = 0;
	cursor_row++;
	if (use_framebuffer && gfx_buf && gfx_cols > 0 && gfx_rows > 0) {
		if (cursor_row >= gfx_rows) {
			/* scroll gfx buffer up one line */
			int line_size = gfx_cols;
			for (int r = 0; r < gfx_rows - 1; r++) {
				char *dst = gfx_buf + r * line_size;
				char *src = gfx_buf + (r + 1) * line_size;
				for (int c = 0; c < line_size; c++)
					dst[c] = src[c];
			}
			/* clear last line */
			char *last = gfx_buf + (gfx_rows - 1) * line_size;
			for (int c = 0; c < line_size; c++)
				last[c] = ' ';
			cursor_row = gfx_rows - 1;
			console_render_text_to_fb();
		}
	} else if (use_framebuffer && !gfx_buf) {
		const bdf_font_t *font = bdf_get_font();
		int fh = font ? (int)font->height : 16;
		int fb_rows = (fh > 0) ? (int)(fb_height / fh) : CONSOLE_ROWS;
		if (cursor_row >= fb_rows) {
			if (use_framebuffer && framebuffer) {
				for (uint32_t y = 0; y < fb_height; y++) {
					for (uint32_t x = 0; x < fb_width;
					     x++) {
						framebuffer[y * fb_pitch + x] =
							fb_bg_color;
					}
				}
			}
			cursor_row = fb_rows - 1;
		}
	} else {
		if (cursor_row >= CONSOLE_ROWS) {
			cursor_row = CONSOLE_ROWS - 1;
		}
	}
}

/**
 * @fn console_putc
 * @brief 位置文字表示
 */
static void console_putc(char ch) {
	if (ch == '\n') {
		new_line();
		serial_putc('\n');
		return;
	}

	if (ch == '\b' || ch == 0x08) {
		// Backspace処理: カーソルを1文字戻す
		if (use_framebuffer && gfx_buf && gfx_cols > 0) {
			if (cursor_col > 0) {
				cursor_col--;
			} else if (cursor_row > 0) {
				cursor_row--;
				cursor_col = gfx_cols - 1;
			}
			int pos = cursor_row * gfx_cols + cursor_col;
			if (pos >= 0 && pos < gfx_cols * gfx_rows)
				gfx_buf[pos] = ' ';
			draw_char_fb(cursor_col, cursor_row, ' ');
		} else {
			if (cursor_col > 0) {
				cursor_col--;
			} else if (cursor_row > 0) {
				// 前の行の最後に移動
				cursor_row--;
				cursor_col = CONSOLE_COLS - 1;
			}
		}
		serial_putc('\b');
		return;
	}

	if (use_framebuffer && gfx_buf && gfx_cols > 0 && gfx_rows > 0) {
		int pos = cursor_row * gfx_cols + cursor_col;
		if (pos >= 0 && pos < gfx_cols * gfx_rows) {
			gfx_buf[pos] = ch;
		}
		draw_char_fb(cursor_col, cursor_row, ch);
		cursor_col++;
		serial_putc(ch);
		if (cursor_col >= gfx_cols) {
			new_line();
		}
	} else {
		if (use_framebuffer && !gfx_buf) {
			const bdf_font_t *font = bdf_get_font();
			int fw = font ? (int)font->width : 8;
			int fb_cols = (fw > 0) ? (int)(fb_width / fw) :
						 CONSOLE_COLS;

			draw_char_fb(cursor_col, cursor_row, ch);
			cursor_col++;
			serial_putc(ch);
			if (cursor_col >= fb_cols) {
				new_line();
			}
		} else {
			/* No VGA/text-mode support: if framebuffer not available,
			 * just emit to serial. */
			serial_putc(ch);
			cursor_col++;
			if (cursor_col >= CONSOLE_COLS) {
				new_line();
			}
		}
	}
}

/* history_offsetからコンソールを再描画 */
static void redraw_from_history(void) {
	/* When using gfx_buf, copy history into it and render. If using direct
	 * framebuffer rendering (no gfx_buf), draw characters directly. If no
	 * framebuffer, do nothing (no VGA support). */
	if (use_framebuffer && gfx_buf && gfx_cols > 0 && gfx_rows > 0) {
		uint32_t gsize = (uint32_t)(gfx_cols * gfx_rows);
		for (uint32_t i = 0; i < gsize; ++i)
			gfx_buf[i] = ' ';

		int start = history_offset;
		for (int r = 0; r < gfx_rows; ++r) {
			int idx = start + r;
			if (idx < 0 || idx >= history_lines)
				continue;
			for (int c = 0; c < gfx_cols; ++c) {
				if (c < CONSOLE_COLS)
					gfx_buf[r * gfx_cols + c] =
						history[idx][c];
				else
					gfx_buf[r * gfx_cols + c] = ' ';
			}
		}
		console_render_text_to_fb();
		return;
	}

	if (use_framebuffer && !gfx_buf) {
		/* Direct framebuffer rendering: paint from history lines */
		const bdf_font_t *font = bdf_get_font();
		if (!font)
			return;
		int fw = (int)font->width;
		int fh = (int)font->height;
		int fb_cols = (fw > 0) ? (int)(fb_width / fw) : CONSOLE_COLS;
		int rows = (fh > 0) ? (int)(fb_height / fh) : CONSOLE_ROWS;
		int start = history_offset;
		for (int r = 0; r < rows; ++r) {
			for (int c = 0; c < fb_cols; ++c) {
				char ch = ' ';
				int idx = start + r;
				if (idx >= 0 && idx < history_lines &&
				    c < CONSOLE_COLS)
					ch = history[idx][c];
				draw_char_fb(c, r, ch);
			}
		}
		return;
	}

	/* No framebuffer: nothing to redraw (VGA/text-mode support removed) */
}

void console_scroll_page_up(void) {
	if (history_lines <= CONSOLE_ROWS)
		return;
	history_offset -= CONSOLE_ROWS;
	if (history_offset < 0)
		history_offset = 0;
	redraw_from_history();
}

void console_scroll_page_down(void) {
	if (history_lines <= CONSOLE_ROWS)
		return;
	int max_offset = history_lines - CONSOLE_ROWS;
	history_offset += CONSOLE_ROWS;
	if (history_offset > max_offset)
		history_offset = max_offset;
	redraw_from_history();
}

void console_render_text_to_fb(void) {
	if (!use_framebuffer)
		return;
	const bdf_font_t *font_info = bdf_get_font();
	if (!font_info)
		return;
	if (gfx_buf && gfx_cols > 0 && gfx_rows > 0) {
		for (int r = 0; r < gfx_rows; ++r) {
			for (int c = 0; c < gfx_cols; ++c) {
				int pos = r * gfx_cols + c;
				char ch = gfx_buf[pos];
				draw_char_fb(c, r, ch);
			}
		}
		return;
	}

	int fb_cols = (int)(fb_width / font_info->width);
	int fb_rows = (int)(fb_height / font_info->height);
	if (fb_cols <= 0 || fb_rows <= 0)
		return;

	int cols = fb_cols;
	int rows = fb_rows;

	/* Render from history (if any) or blank when no gfx_buf exists. */
	for (int r = 0; r < rows; ++r) {
		for (int c = 0; c < cols; ++c) {
			char ch = ' ';
			int idx = history_offset + r;
			if (idx >= 0 && idx < history_lines &&
			    c < CONSOLE_COLS) {
				ch = history[idx][c];
			}
			draw_char_fb(c, r, ch);
		}
	}
}

void console_post_font_init(void) {
	allocate_gfx_buf_if_needed();
	if (gfx_buf && gfx_cols > 0 && gfx_rows > 0) {
		/* Initialize gfx_buf from history if available, otherwise blank */
		int copy_rows = (gfx_rows < CONSOLE_ROWS) ? gfx_rows :
							    CONSOLE_ROWS;
		int copy_cols = (gfx_cols < CONSOLE_COLS) ? gfx_cols :
							    CONSOLE_COLS;
		for (int r = 0; r < copy_rows; ++r) {
			int idx = history_offset + r;
			for (int c = 0; c < copy_cols; ++c) {
				if (idx >= 0 && idx < history_lines)
					gfx_buf[r * gfx_cols + c] =
						history[idx][c];
				else
					gfx_buf[r * gfx_cols + c] = ' ';
			}
			for (int c = copy_cols; c < gfx_cols; ++c) {
				gfx_buf[r * gfx_cols + c] = ' ';
			}
		}
		for (int r = copy_rows; r < gfx_rows; ++r) {
			for (int c = 0; c < gfx_cols; ++c)
				gfx_buf[r * gfx_cols + c] = ' ';
		}

		console_render_text_to_fb();
	}
}

/**
 * @brief 画面全体をクリア
 */
void console_clear_screen(void) {
	/* Clear gfx buffer (if any) and framebuffer. Legacy VGA text buffer
	 * is not used in UEFI builds. */
	if (gfx_buf && gfx_cols > 0 && gfx_rows > 0) {
		uint32_t size = (uint32_t)(gfx_cols * gfx_rows);
		for (uint32_t i = 0; i < size; ++i)
			gfx_buf[i] = ' ';
	}

	if (use_framebuffer && framebuffer) {
		uint32_t color = fb_bg_color;
		for (uint32_t y = 0; y < fb_height; ++y) {
			for (uint32_t x = 0; x < fb_width; ++x) {
				framebuffer[y * fb_pitch + x] = color;
			}
		}
	}

	cursor_row = 0;
	cursor_col = 0;
}

/**
 * @fn console_write
 * @brief 文字列を書き込む
 */
static void console_write(const char *s) {
	while (*s) {
		console_putc(*s++);
	}
}

/**
 * @fn printk
 * @brief printfのようにVGAに出力
 */
int printk(const char *fmt, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	int i = 0, j = 0;

	while (fmt[i] && j < (int)sizeof(buffer) - 1) {
		if (fmt[i] == '%' && fmt[i + 1]) {
			i++;
			// widthとパディングの簡易サポート
			int width = 0;
			int pad_zero = 0;
			int left_align = 0; // 左詰めフラグ

			// '-' フラグをチェック（左詰め）
			if (fmt[i] == '-') {
				left_align = 1;
				i++;
			}

			if (fmt[i] == '0') {
				pad_zero = 1;
				i++;
				while (fmt[i] >= '0' && fmt[i] <= '9') {
					width = width * 10 + (fmt[i] - '0');
					i++;
				}
			} else {
				while (fmt[i] >= '0' && fmt[i] <= '9') {
					width = width * 10 + (fmt[i] - '0');
					i++;
				}
			}

			char spec = fmt[i];
			if (spec == 'd') {
				int val = va_arg(args, int);
				char numbuf[32];
				int n = 0, k = 0;
				unsigned int uval;
				if (val < 0) {
					buffer[j++] = '-';
					uval = (unsigned int)(-val);
				} else {
					uval = (unsigned int)val;
				}
				do {
					numbuf[n++] = '0' + (uval % 10);
					uval /= 10;
				} while (uval && n < (int)sizeof(numbuf));
				if (width > n) {
					int pad = width - n;
					while (pad-- > 0 &&
					       j < (int)sizeof(buffer) - 1)
						buffer[j++] = pad_zero ? '0' :
									 ' ';
				}
				for (k = n - 1;
				     k >= 0 && j < (int)sizeof(buffer) - 1; k--)
					buffer[j++] = numbuf[k];
			} else if (spec == 'u') {
				unsigned int val = va_arg(args, unsigned int);
				char numbuf[32];
				int n = 0, k = 0;
				do {
					numbuf[n++] = '0' + (val % 10);
					val /= 10;
				} while (val && n < (int)sizeof(numbuf));
				if (width > n) {
					int pad = width - n;
					while (pad-- > 0 &&
					       j < (int)sizeof(buffer) - 1)
						buffer[j++] = pad_zero ? '0' :
									 ' ';
				}
				for (k = n - 1;
				     k >= 0 && j < (int)sizeof(buffer) - 1; k--)
					buffer[j++] = numbuf[k];
			} else if (spec == 'l' && fmt[i + 1] == 'l' &&
				   fmt[i + 2] == 'u') {
				i += 2;
				uint64_t val = va_arg(args, uint64_t);
				char numbuf[32];
				int n = 0, k = 0;
				if (val == 0) {
					numbuf[n++] = '0';
				} else {
					while (val && n < (int)sizeof(numbuf)) {
						numbuf[n++] = '0' + (val % 10);
						val /= 10;
					}
				}
				if (width > n) {
					int pad = width - n;
					while (pad-- > 0 &&
					       j < (int)sizeof(buffer) - 1)
						buffer[j++] = pad_zero ? '0' :
									 ' ';
				}
				for (k = n - 1;
				     k >= 0 && j < (int)sizeof(buffer) - 1; k--)
					buffer[j++] = numbuf[k];
			} else if (spec == 'l' &&
				   (fmt[i + 1] == 'x' || fmt[i + 1] == 'X')) {
				i++;
				uint64_t val = va_arg(args, uint64_t);
				char numbuf[32];
				int n = 0, k = 0;
				const char *hex = (fmt[i] == 'x') ?
							  "0123456789abcdef" :
							  "0123456789ABCDEF";
				if (val == 0) {
					numbuf[n++] = '0';
				} else {
					while (val && n < (int)sizeof(numbuf)) {
						numbuf[n++] = hex[val & 0xF];
						val >>= 4;
					}
				}
				if (width > n) {
					int pad = width - n;
					while (pad-- > 0 &&
					       j < (int)sizeof(buffer) - 1)
						buffer[j++] = pad_zero ? '0' :
									 ' ';
				}
				for (k = n - 1;
				     k >= 0 && j < (int)sizeof(buffer) - 1; k--)
					buffer[j++] = numbuf[k];
			} else if (spec == 'x' || spec == 'X') {
				unsigned int val = va_arg(args, unsigned int);
				char numbuf[32];
				int n = 0, k = 0;
				const char *hex = (spec == 'x') ?
							  "0123456789abcdef" :
							  "0123456789ABCDEF";
				do {
					numbuf[n++] = hex[val & 0xF];
					val >>= 4;
				} while (val && n < (int)sizeof(numbuf));
				if (width > n) {
					int pad = width - n;
					while (pad-- > 0 &&
					       j < (int)sizeof(buffer) - 1)
						buffer[j++] = pad_zero ? '0' :
									 ' ';
				}
				for (k = n - 1;
				     k >= 0 && j < (int)sizeof(buffer) - 1; k--)
					buffer[j++] = numbuf[k];
			} else if (spec == 's') {
				const char *s = va_arg(args, const char *);
				int len = 0;
				const char *p = s;
				// 文字列の長さを計算
				while (*p) {
					len++;
					p++;
				}
				// 左詰めの場合: 文字列を先に出力してからパディング
				if (left_align) {
					while (*s &&
					       j < (int)sizeof(buffer) - 1)
						buffer[j++] = *s++;
					// 右側にパディング
					if (width > len) {
						int pad = width - len;
						while (pad-- > 0 &&
						       j < (int)sizeof(buffer) -
								       1)
							buffer[j++] = ' ';
					}
				} else {
					// 右詰め（デフォルト）: パディングしてから文字列
					if (width > len) {
						int pad = width - len;
						while (pad-- > 0 &&
						       j < (int)sizeof(buffer) -
								       1)
							buffer[j++] = ' ';
					}
					while (*s &&
					       j < (int)sizeof(buffer) - 1)
						buffer[j++] = *s++;
				}
			} else if (spec == 'c') {
				char c = (char)va_arg(args, int);
				buffer[j++] = c;
			} else if (spec == 'p') {
				void *ptr = va_arg(args, void *);
				uintptr_t val = (uintptr_t)ptr;
				if (j < (int)sizeof(buffer) - 1)
					buffer[j++] = '0';
				if (j < (int)sizeof(buffer) - 1)
					buffer[j++] = 'x';
				const char *hex = "0123456789abcdef";
				int nibbles = (int)sizeof(uintptr_t) * 2;
				for (int k = nibbles - 1; k >= 0; k--) {
					int shift = k * 4;
					uint8_t d = (val >> shift) & 0xF;
					if (j < (int)sizeof(buffer) - 1)
						buffer[j++] = hex[d];
				}
			} else {
				buffer[j++] = '%';
				buffer[j++] = spec;
			}
		} else if (fmt[i] == '\\' && fmt[i + 1] == 'n') {
			buffer[j++] = '\n';
			i++;
		} else if (fmt[i] == '\n') {
			buffer[j++] = '\n';
		} else {
			buffer[j++] = fmt[i];
		}
		i++;
	}
	buffer[j] = '\0';
	va_end(args);

	uint32_t _irq_flags = irq_save();
	console_write(buffer);
	irq_restore(_irq_flags);
	return j;
}