#include <stdint.h>
#include <stddef.h>
#include <util/console.h>
#include <mem/manager.h>
#include <mem/paging.h>
#include <mem/vmem.h>
#include <mem/map.h>
#include <task/multi_task.h>
#include <fs/vfs.h>
#include <task/elf.h>

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

	void *buf = NULL;
	uint32_t size = 0;
	int r = vfs_read_file_all(path, &buf, &size);
	if (r != 0 || !buf || size < sizeof(Elf64_Ehdr)) {
		if (buf) {
			kfree(buf);
			buf = NULL;
		}

		int is_dir = 0;
		int rr = vfs_resolve_path(path, &is_dir, &size);
		if (rr != 0 || is_dir || size == 0) {
			printk("elf_run: failed to read '%s' (err=%d)\n", path,
			       r);
			return -2;
		}
		buf = (void *)kmalloc((size_t)size + 1);
		if (!buf) {
			printk("elf_run: kmalloc failed for size %u\n", size);
			return -2;
		}
		int fd = vfs_open(path, 0, 0);
		if (fd < 0) {
			printk("elf_run: vfs_open failed for '%s' (fd=%d)\n",
			       path, fd);
			kfree(buf);
			return -2;
		}
		uint32_t read_total = 0;
		while (read_total < size) {
			int got = vfs_read(
				fd, (void *)((uintptr_t)buf + read_total),
				size - read_total);
			if (got <= 0)
				break;
			read_total += (uint32_t)got;
		}
		vfs_close(fd);
		if (read_total == 0) {
			printk("elf_run: vfs_read returned 0 for '%s'\n", path);
			kfree(buf);
			return -2;
		}
		((uint8_t *)buf)[read_total] = '\0';
		size = read_total;
	}

	Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
		printk("elf_run: not an ELF: %s\n", path);
		kfree(buf);
		return -3;
	}

	/* create user-mode task but do NOT ready it until mapping is done */
	task_t *t = task_create((void *)0x0, path, 0);
	if (!t) {
		printk("elf_run: task_create failed\n");
		kfree(buf);
		return -4;
	}

	uint32_t pd_phys = (uint32_t)t->page_directory;
	if (pd_phys == 0) {
		printk("elf_run: invalid page directory for task\n");
		kfree(buf);
		return -5;
	}

	Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)buf + eh->e_phoff);
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
				kfree(buf);
				return -6;
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
				kfree(buf);
				return -7;
			}

			uint32_t frame_virt = vmem_phys_to_virt(phys);
			if (frame_virt == 0) {
				printk("elf_run: vmem_phys_to_virt failed for phys=0x%x\n",
				       phys);
				kfree(buf);
				return -8;
			}
			uint8_t *dst = (uint8_t *)(uintptr_t)frame_virt;

			uint32_t page_va = dest_virt;
			uint64_t seg_file_end = off + filesz;
			if ((uint64_t)page_va + 0x1000 <= vaddr) {
				for (uint32_t z = 0; z < 0x1000; ++z)
					dst[z] = 0;
			} else {
				uint64_t file_page_offset = 0;
				if ((uint64_t)vaddr > (uint64_t)page_va)
					file_page_offset = 0;
				else
					file_page_offset =
						(uint64_t)(page_va - vaddr);
				uint64_t src = off + file_page_offset;
				uint64_t max_copy = 0;
				if (src < seg_file_end) {
					max_copy = seg_file_end - src;
					if (max_copy > 0x1000)
						max_copy = 0x1000;
					uint32_t dst_off =
						(uint32_t)(((uint64_t)vaddr >
							    (uint64_t)page_va) ?
								   ((uint64_t)
									    vaddr -
								    (uint64_t)
									    page_va) :
								   0);
					uint8_t *srcp =
						(uint8_t *)buf + (size_t)src;
					for (uint32_t z = 0;
					     z < (uint32_t)max_copy; ++z)
						dst[dst_off + z] = srcp[z];
					for (uint32_t z = dst_off +
							  (uint32_t)max_copy;
					     z < 0x1000; ++z)
						dst[z] = 0;
				} else {
					for (uint32_t z = 0; z < 0x1000; ++z)
						dst[z] = 0;
				}
			}
		}
	}

	uint32_t entry = (uint32_t)(eh->e_entry & 0xFFFFFFFFu);
	t->regs.rip = (uint64_t)entry;

	task_ready(t);
	kfree(buf);
	return 0;
}