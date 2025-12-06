#include <driver/ata.h>
#include <util/io.h>
#include <util/console.h>
#include <interrupt/irq.h>

/* Detected drive id recorded after successful ata_init() */
static int g_ata_detected_drive = -1;

int ata_get_detected_drive(void) {
	return g_ata_detected_drive;
}

/**
 * @brief ATAデバイスの準備完了を待つ
 */
static int ata_wait_ready(uint16_t base) {
	uint32_t timeout = 100000;
	uint8_t status = 0;

	while (timeout--) {
		status = inb(base + 7);
		if (!(status & ATA_SR_BSY)) {
			return 0; /* ready */
		}

		/* 割り込みイベントを頻繁に処理してFIFO溢れを防ぐ */
		if ((timeout % 100) == 0) {
			interrupt_dispatch_all();
		}
	}

	return -1;
}

/**
 * @brief ATAデバイスのDRQビットを待つ
 */
static int ata_wait_drq(uint16_t base) {
	uint32_t timeout = 100000;
	uint8_t status = 0;

	while (timeout--) {
		status = inb(base + 7);

		/* エラーが出たら即時報告 */
		if (status & ATA_SR_ERR) {
			printk("ATA: ata_wait_drq detected ERR (status=0x%x, base=0x%x)\n",
			       status, base);
			return -1; /* エラー */
		}

		if (status & ATA_SR_DRQ) {
			return 0; /* DRQ準備完了 */
		}

		/* 割り込みイベントを頻繁に処理してFIFO溢れを防ぐ */
		if ((timeout % 100) == 0) {
			interrupt_dispatch_all();
		}
	}

	return -1; /* タイムアウト */
}

/**
 * @brief ドライブ番号からベースアドレスとドライブ選択を取得
 */
static void ata_get_base(uint8_t drive, uint16_t *base, uint8_t *drive_sel) {
	if (drive < 2) {
		/* Primary Bus */
		*base = ATA_PRIMARY_DATA;
		*drive_sel = (drive == 0) ? ATA_MASTER : ATA_SLAVE;
	} else {
		/* Secondary Bus */
		*base = ATA_SECONDARY_DATA;
		*drive_sel = (drive == 2) ? ATA_MASTER : ATA_SLAVE;
	}
}

/**
 * @brief ATAドライバを初期化する
 */
int ata_init(void) {
#ifdef INIT_MSG
	printk("ATA: Initializing ATA driver\n");
#endif
	/* 試すドライブのリスト */
	const struct {
		uint16_t base;
		uint8_t drive_sel;
		uint8_t drive_id; /* drive id used by ata_read_sectors */
		const char *name;
	} drives[] = {
		{ ATA_PRIMARY_DATA, ATA_SLAVE, 1, "Primary Slave (hdb)" },
		{ ATA_SECONDARY_DATA, ATA_MASTER, 2, "Secondary Master (hdc)" },
		{ ATA_PRIMARY_DATA, ATA_MASTER, 0, "Primary Master (hda)" },
	};

	for (int i = 0; i < 3; i++) {
		/* 割り込みを無効化（PIOモードでポーリングするため） */
		outb(drives[i].base + 0x206, 0x02); /* nIEN ビットをセット */

		/* ドライブを選択 */
		outb(drives[i].base + 6, drives[i].drive_sel);

		/* 400ns待つ（ポートを4回読む） */
		for (int j = 0; j < 4; j++) {
			inb(drives[i].base + 7);
		}

		/* IDENTIFYコマンドを送信 */
		outb(drives[i].base + 7, ATA_CMD_IDENTIFY);

		/* 400ns待つ */
		for (int j = 0; j < 4; j++) {
			inb(drives[i].base + 7);
		}

		/* ステータスを確認 */
		uint8_t status = inb(drives[i].base + 7);

		/* ドライブが存在しない */
		if (status == 0 || status == 0xFF) {
#ifdef INIT_MSG
			printk("ATA:   No drive (status=0x%x)\n", status);
#endif
			continue;
		}

		/* エラーチェック */
		if (status & ATA_SR_ERR) {
			uint8_t err = inb(drives[i].base + 1);
#ifdef INIT_MSG
			printk("ATA:   Error detected (err=0x%x)\n", err);
#endif
			/* ATAPI デバイスの場合はスキップ */
			if (err == 0x01) {
#ifdef INIT_MSG
				printk("ATA:   ATAPI device (not supported)\n");
#endif
			}
			continue;
		}

		/* BSYがクリアされるまで待つ */
		uint32_t timeout = 100000;
		while (timeout-- && (inb(drives[i].base + 7) & ATA_SR_BSY)) {
			/* 割り込みイベントを処理 */
			if ((timeout % 100) == 0) {
				interrupt_dispatch_all();
			}
		}

		if (timeout == 0) {
			printk("ATA:   Timeout waiting for BSY clear (base=0x%x)\n",
			       drives[i].base);
			continue;
		}

		/* DRQビットを待つ */
		timeout = 100000;
		while (timeout-- && !(inb(drives[i].base + 7) & ATA_SR_DRQ)) {
			/* 割り込みイベントを処理 */
			if ((timeout % 100) == 0) {
				interrupt_dispatch_all();
			}
		}

		if (timeout == 0) {
			printk("ATA:   Timeout waiting for DRQ (base=0x%x)\n",
			       drives[i].base);
			continue;
		}
#ifdef INIT_MSG
		/* IDENTIFYデータを読み取る（256ワード = 512バイト）*/
		printk("ATA:   reading IDENTIFY data from base 0x%x\n",
		       drives[i].base);
#endif
#ifdef INIT_MSG
		for (int j = 0; j < 256; j++) {
			(void)inw(drives[i].base);
		}
#else
		for (int j = 0; j < 256; j++) {
			(void)inw(drives[i].base);
		}
#endif
#ifdef INIT_MSG
#endif
		g_ata_detected_drive = drives[i].drive_id;
#ifdef INIT_MSG
		printk("ATA: %s detected successfully! (drive=%u)\n",
		       drives[i].name, g_ata_detected_drive);
#endif
		return 0;
	}

	printk("ATA: No valid ATA drive found\n");
	return -1;
}

/**
 * @brief ATAデバイスからセクタを読み取る
 */
int ata_read_sectors(uint8_t drive, uint32_t lba, uint8_t sectors,
		     void *buffer) {
	uint16_t base;
	uint8_t drive_sel;
	uint16_t *buf = (uint16_t *)buffer;

	if (sectors == 0) {
		return -1;
	}

	/* ベースアドレスとドライブ選択を取得 */
	ata_get_base(drive, &base, &drive_sel);

	/* Diagnostic: announce ATA read operation start */
	printk("ATA: ata_read_sectors start (drive=%u lba=%u sectors=%u)\n",
	       drive, lba, sectors);

	/* 割り込みを無効化 */
	outb(base + 0x206, 0x02);

	/* デバイスが準備完了するまで待機 */
	if (ata_wait_ready(base) != 0) {
		printk("ATA: device not ready (base=0x%x)\n", base);
		return -1;
	}

	/* LBA28モードでドライブを選択 */
	outb(base + 6, (drive_sel | 0xE0) | ((lba >> 24) & 0x0F));

	/* セクタ数を設定 */
	outb(base + 2, sectors);

	/* LBAアドレスを設定 */
	outb(base + 3, (uint8_t)(lba & 0xFF));
	outb(base + 4, (uint8_t)((lba >> 8) & 0xFF));
	outb(base + 5, (uint8_t)((lba >> 16) & 0xFF));

	/* READコマンドを送信 */
	outb(base + 7, ATA_CMD_READ_PIO);
	printk("ATA: READ_PIO sent (base=0x%x lba=%u sectors=%u drive=%u)\n",
	       base, lba, sectors, drive);

	/* 各セクタを読み取る */
	for (int s = 0; s < sectors; s++) {
		/* DRQを待つ */
		if (ata_wait_drq(base) != 0) {
			printk("ATA: Read error/timeout at sector %d (base=0x%x lba=%u drive=%u)\n",
			       s, base, lba + s, drive);
			printk("ATA: ata_read_sectors failing (drive=%u lba=%u sectors=%u)\n",
			       drive, lba, sectors);
			return -1;
		}

		/* 256ワード（512バイト）を読み取る */
		for (int i = 0; i < 256; i++) {
			buf[s * 256 + i] = inw(base);
		}

		/* 400nsの遅延 */
		for (int i = 0; i < 4; i++) {
			inb(base + 7);
		}

		/* セクタ読み取り後に割り込みを処理 */
		interrupt_dispatch_all();
	}

	return 0;
}

/**
 * @brief ATAデバイスにセクタを書き込む
 */
int ata_write_sectors(uint8_t drive, uint32_t lba, uint8_t sectors,
		      const void *buffer) {
	uint16_t base;
	uint8_t drive_sel;
	const uint16_t *buf = (const uint16_t *)buffer;

	if (sectors == 0) {
		return -1;
	}

	/* ベースアドレスとドライブ選択を取得 */
	ata_get_base(drive, &base, &drive_sel);

	/* 割り込みを無効化 */
	outb(base + 0x206, 0x02);

	/* デバイスが準備完了するまで待機 */
	if (ata_wait_ready(base) != 0) {
		printk("ATA: device not ready before write (base=0x%x)\n",
		       base);
		return -1;
	}

	/* LBA28モードでドライブを選択 */
	outb(base + 6, (drive_sel | 0xE0) | ((lba >> 24) & 0x0F));

	/* セクタ数を設定 */
	outb(base + 2, sectors);

	/* LBAアドレスを設定 */
	outb(base + 3, (uint8_t)(lba & 0xFF));
	outb(base + 4, (uint8_t)((lba >> 8) & 0xFF));
	outb(base + 5, (uint8_t)((lba >> 16) & 0xFF));

	/* WRITEコマンドを送信 */
	outb(base + 7, ATA_CMD_WRITE_PIO);

	/* 各セクタを書き込む */
	for (int s = 0; s < sectors; s++) {
		/* DRQを待つ */
		if (ata_wait_drq(base) != 0) {
			printk("ATA: Write error/timeout at sector %d (base=0x%x lba=%u)\n",
			       s, base, lba + s);
			return -1;
		}

		/* 256ワード（512バイト）を書き込む */
		for (int i = 0; i < 256; i++) {
			outw(base, buf[s * 256 + i]);
		}

		/* 書き込み完了を待つ */
		ata_wait_ready(base);

		/* 400nsの遅延 */
		for (int i = 0; i < 4; i++) {
			inb(base + 7);
		}
	}

	return 0;
}
