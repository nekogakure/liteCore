#include <stdint.h>
#include <stddef.h>
#include <util/console.h>
#include <mem/manager.h>
#include <mem/paging.h>
#include <mem/vmem.h>
#include <mem/map.h>
#include <task/multi_task.h>
#include <task/elf.h>
#include <fs/vfs.h>

typedef unsigned long Elf64_Addr;
typedef unsigned long Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;

typedef struct {
	unsigned char e_ident[16];
	Elf64_Half e_type;
	Elf64_Half e_machine;
	Elf64_Word e_version;
	Elf64_Addr e_entry;
	Elf64_Off e_phoff;
	Elf64_Off e_shoff;
	Elf64_Word e_flags;
	Elf64_Half e_ehsize;
	Elf64_Half e_phentsize;
	Elf64_Half e_phnum;
	Elf64_Half e_shentsize;
	Elf64_Half e_shnum;
	Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	Elf64_Word p_type;
	Elf64_Word p_flags;
	Elf64_Off p_offset;
	Elf64_Addr p_vaddr;
	Elf64_Addr p_paddr;
	Elf64_Xword p_filesz;
	Elf64_Xword p_memsz;
	Elf64_Xword p_align;
} Elf64_Phdr;

int elf_run(const char *path) {
	if (!path)
		return -1;

	/* Open file for streaming read */
	int fd = vfs_open(path, 0, 0);
	if (fd < 0) {
		printk("elf_run: vfs_open failed for '%s' (fd=%d)\n", path, fd);
		return -2;
	}

	/* Read ELF header only (small allocation) */
	Elf64_Ehdr eh_buf;
	int hdr_read = vfs_read(fd, &eh_buf, sizeof(Elf64_Ehdr));
	if (hdr_read < (int)sizeof(Elf64_Ehdr)) {
		printk("elf_run: failed to read ELF header from '%s'\n", path);
		vfs_close(fd);
		return -2;
	}

	Elf64_Ehdr *eh = &eh_buf;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
		printk("elf_run: not an ELF: %s\n", path);
		vfs_close(fd);
		return -3;
	}

	/* Read program headers (still small) */
	uint32_t ph_size = eh->e_phnum * sizeof(Elf64_Phdr);
	if (ph_size == 0 || ph_size > 4096) {
		printk("elf_run: invalid program header size %u\n", ph_size);
		vfs_close(fd);
		return -3;
	}

	Elf64_Phdr *ph = (Elf64_Phdr *)kmalloc(ph_size + 16);
	if (!ph) {
		printk("elf_run: kmalloc failed for program headers\n");
		vfs_close(fd);
		return -3;
	}

	vfs_lseek(fd, eh->e_phoff, 0);
	int ph_read = vfs_read(fd, ph, ph_size);
	if (ph_read < (int)ph_size) {
		printk("elf_run: failed to read program headers\n");
		kfree(ph);
		vfs_close(fd);
		return -3;
	}

	/* create user-mode task but do NOT ready it until mapping is done */
	task_t *t = task_create((void *)0x0, path, 0);
	if (!t) {
		printk("elf_run: task_create failed\n");
		kfree(ph);
		vfs_close(fd);
		return -4;
	}

	uint32_t pd_phys = (uint32_t)t->page_directory;
	if (pd_phys == 0) {
		printk("elf_run: invalid page directory for task\n");
		kfree(ph);
		vfs_close(fd);
		return -5;
	}

	/* Allocate a single page buffer for streaming reads (4KB) */
	uint8_t *read_buf = (uint8_t *)kmalloc(4096 + 16);
	if (!read_buf) {
		printk("elf_run: kmalloc failed for read buffer\n");
		kfree(ph);
		vfs_close(fd);
		return -6;
	}

	/* Process each PT_LOAD segment with streaming reads */
	for (int i = 0; i < eh->e_phnum; ++i) {
		if (ph[i].p_type != 1) /* PT_LOAD */
			continue;

		Elf64_Addr vaddr = (Elf64_Addr)ph[i].p_vaddr;
		uint64_t filesz = (uint64_t)ph[i].p_filesz;
		uint64_t memsz = (uint64_t)ph[i].p_memsz;
		uint64_t off = (uint64_t)ph[i].p_offset;

		uint32_t page_off = (uint32_t)(vaddr & 0xFFF);
		uint32_t map_start = (uint32_t)(vaddr & ~0xFFFUL);
		uint32_t to_map =
			(uint32_t)(((page_off + memsz) + 0xFFF) & ~0xFFFUL);

		uint32_t pages = to_map / 0x1000;
		for (uint32_t p = 0; p < pages; ++p) {
			void *frame = alloc_frame();
			if (!frame) {
				printk("elf_run: alloc_frame failed for segment\n");
				kfree(read_buf);
				kfree(ph);
				vfs_close(fd);
				return -7;
			}
			uint32_t phys = (uint32_t)(uintptr_t)frame;
			uint32_t dest_virt = map_start + p * 0x1000;

			/* map the page into the new page directory */
			int flags = PAGING_PRESENT | PAGING_USER;
			/* set RW if segment is writable */
			if (ph[i].p_flags & 0x2) /* PF_W */
				flags |= PAGING_RW;

			int mf = map_page_pd(pd_phys, phys, dest_virt, flags);
			if (mf != 0) {
				printk("elf_run: map_page_pd failed for va=0x%x\n",
				       dest_virt);
				kfree(read_buf);
				kfree(ph);
				vfs_close(fd);
				return -8;
			}

			uint32_t frame_virt = vmem_phys_to_virt(phys);
			if (frame_virt == 0) {
				printk("elf_run: vmem_phys_to_virt failed for phys=0x%x\n",
				       phys);
				kfree(read_buf);
				kfree(ph);
				vfs_close(fd);
				return -9;
			}
			uint8_t *dst = (uint8_t *)(uintptr_t)frame_virt;

			/* Calculate what to copy to this page */
			uint32_t page_va = dest_virt;
			uint64_t seg_file_end = off + filesz;

			/* Initialize entire page to zero first */
			for (uint32_t z = 0; z < 0x1000; ++z)
				dst[z] = 0;

			/* Only copy file data if this page overlaps with file content */
			if ((uint64_t)page_va < (uint64_t)vaddr + filesz &&
			    (uint64_t)page_va + 0x1000 > (uint64_t)vaddr) {
				/* Calculate file offset for this page */
				uint64_t page_file_start = 0;
				uint32_t dst_off = 0;

				if ((uint64_t)page_va < (uint64_t)vaddr) {
					/* Page starts before segment */
					dst_off = (uint32_t)((uint64_t)vaddr -
							     (uint64_t)page_va);
					page_file_start = off;
				} else {
					/* Page starts within segment */
					page_file_start =
						off + ((uint64_t)page_va -
						       (uint64_t)vaddr);
				}

				/* Calculate how many bytes to copy */
				uint64_t bytes_remaining =
					seg_file_end - page_file_start;
				uint32_t to_copy =
					(uint32_t)((bytes_remaining >
						    (0x1000 - dst_off)) ?
							   (0x1000 - dst_off) :
							   bytes_remaining);

				if (to_copy > 0) {
					/* Seek to file position and read */
					vfs_lseek(fd, page_file_start, 0);
					int got =
						vfs_read(fd, read_buf, to_copy);
					if (got > 0) {
						/* Copy from read buffer to destination */
						for (int z = 0; z < got; ++z)
							dst[dst_off + z] =
								read_buf[z];
					}
				}
			}
		}
	}

	uint32_t entry = (uint32_t)(eh->e_entry & 0xFFFFFFFFu);
	t->regs.rip = (uint64_t)entry;

	task_ready(t);
	kfree(read_buf);
	kfree(ph);
	vfs_close(fd);
	return 0;
}