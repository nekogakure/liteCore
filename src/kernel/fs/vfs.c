#include <fs/vfs.h>
#include <device/keyboard.h>
#include <util/console.h>
#include <mem/usercopy.h>
#include <task/multi_task.h>
#include <stdint.h>
#include <stddef.h>
#include <mem/manager.h>
#include <fs/ext/ext2.h>
#include <fs/fat/fat16.h>
#include <fs/block_cache.h>

#define MAX_OPEN_FILES 2048
#define MAX_BACKENDS 128

struct vfs_backend {
	const char *name;
	/* mount returns backend-specific sb pointer via out_sb */
	int (*mount_with_cache)(struct block_cache *cache, void **out_sb);
	int (*read_file)(void *sb, const char *path, void *buf, size_t len,
			 size_t *out_len);
	int (*write_file)(void *sb, const char *path, const void *buf,
			  size_t len);
	int (*get_file_size)(void *sb, const char *path, uint32_t *out_size);
};

static struct vfs_backend *backends[MAX_BACKENDS];
static int backend_count = 0;
static struct vfs_backend *active_backend = NULL;
static void *active_sb = NULL;

typedef enum { VFS_TYPE_UNKNOWN = 0, VFS_TYPE_GENERIC } vfs_type_t;

struct vfs_file {
	vfs_type_t type;
	/* generic backend storage */
	void *sb; /* backend superblock pointer */
	char path[256];
	uint8_t *buf; /* full file contents cached */
	uint32_t buf_size; /* actual file content size */
	uint32_t buf_allocated; /* allocated buffer size (for safety checks) */
	uint32_t offset;
};

static struct vfs_file *open_files[MAX_OPEN_FILES];

static int allocate_global_handle(struct vfs_file *f) {
	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		if (open_files[i] == NULL) {
			open_files[i] = f;
			return i;
		}
	}
	return -1;
}

static void free_global_handle(int idx) {
	if (idx < 0 || idx >= MAX_OPEN_FILES)
		return;
	if (open_files[idx]) {
		if (open_files[idx]->buf)
			kfree(open_files[idx]->buf);
		kfree(open_files[idx]);
		open_files[idx] = NULL;
	}
}

void vfs_init(void) {
	/* intentionally empty: backends can be registered explicitly */
}

/* backend registration */
int vfs_register_backend(struct vfs_backend *b) {
	if (!b || backend_count >= MAX_BACKENDS)
		return -1;
	backends[backend_count++] = b;
	return 0;
}

/* internal helpers to wrap existing ext2/fat16 APIs */
static int ext2_mount_wrapper(struct block_cache *cache, void **out_sb) {
	struct ext2_super *s = NULL;
	if (ext2_mount_with_cache(cache, &s) == 0) {
		*out_sb = (void *)s;
		return 0;
	}
	return -1;
}

static int ext2_read_wrapper(void *sb, const char *path, void *buf, size_t len,
			     size_t *out_len) {
	struct ext2_super *s = (struct ext2_super *)sb;
	if (!s || !path || !buf)
		return -1;
	uint32_t inode_num;
	if (ext2_resolve_path(s, path, &inode_num) != 0)
		return -2;
	struct ext2_inode inode;
	if (ext2_read_inode(s, inode_num, &inode) != 0)
		return -3;
	size_t read = 0;
	if (ext2_read_inode_data(s, &inode, buf, len, 0, &read) != 0)
		return -4;
	if (out_len)
		*out_len = read;
	return 0;
}

static int ext2_get_size_wrapper(void *sb, const char *path,
				 uint32_t *out_size) {
	struct ext2_super *s = (struct ext2_super *)sb;
	if (!s || !path || !out_size)
		return -1;
	uint32_t inode_num;
	if (ext2_resolve_path(s, path, &inode_num) != 0)
		return -2;
	struct ext2_inode inode;
	if (ext2_read_inode(s, inode_num, &inode) != 0)
		return -3;
	*out_size = inode.i_size;
	return 0;
}

static int fat16_mount_wrapper(struct block_cache *cache, void **out_sb) {
	struct fat16_super *s = NULL;
	if (fat16_mount_with_cache(cache, &s) == 0) {
		*out_sb = (void *)s;
		return 0;
	}
	return -1;
}

static int fat16_read_wrapper(void *sb, const char *path, void *buf, size_t len,
			      size_t *out_len) {
	struct fat16_super *s = (struct fat16_super *)sb;
	if (!s || !path || !buf)
		return -1;
	return fat16_read_file(s, path, buf, len, out_len);
}

static int fat16_write_wrapper(void *sb, const char *path, const void *buf,
			       size_t len) {
	struct fat16_super *s = (struct fat16_super *)sb;
	if (!s || !path || !buf)
		return -1;
	return fat16_write_file(s, path, buf, len);
}

static int fat16_get_size_wrapper(void *sb, const char *path,
				  uint32_t *out_size) {
	struct fat16_super *s = (struct fat16_super *)sb;
	if (!s || !path || !out_size)
		return -1;
	return fat16_get_file_size(s, path, out_size);
}

/* register built-in backends (call from init_msg before mount) */
void vfs_register_builtin_backends(void) {
	static struct vfs_backend ext2b = {
		.name = "ext2",
		.mount_with_cache = ext2_mount_wrapper,
		.read_file = ext2_read_wrapper,
		.write_file = NULL,
		.get_file_size = ext2_get_size_wrapper,
	};
	static struct vfs_backend fat16b = {
		.name = "fat16",
		.mount_with_cache = fat16_mount_wrapper,
		.read_file = fat16_read_wrapper,
		.write_file = fat16_write_wrapper,
		.get_file_size = fat16_get_size_wrapper,
	};
	/* prefer FAT16 first (project preference) */
	vfs_register_backend(&fat16b);
	vfs_register_backend(&ext2b);
}

/* Try to mount any registered backend using the provided block cache. */
int vfs_mount_with_cache(struct block_cache *cache) {
	if (!cache)
		return -1;

	for (int i = 0; i < backend_count; ++i) {
		void *sb = NULL;
		if (!backends[i])
			continue;
		int r = -1;
		if (backends[i]->mount_with_cache)
			r = backends[i]->mount_with_cache(cache, &sb);
		if (r == 0) {
			active_backend = backends[i];
			active_sb = sb;
			return 0;
		}
	}
	return -2; /* no backend mounted */
}

/* Per-task fd allocation helpers */
static int vfs_fd_alloc_for_current(void) {
	task_t *t = task_current();
	if (!t)
		return -1;
	for (int i = 3; i < 32; ++i) {
		if (t->fds[i] == -1) {
			return i;
		}
	}
	return -1;
}

static int vfs_fd_release_for_current(int fd) {
	task_t *t = task_current();
	if (!t)
		return -1;
	if (fd < 3 || fd >= 32)
		return -1;
	if (t->fds[fd] == -1)
		return -1;
	int global_idx = t->fds[fd];
	if (global_idx >= 0) {
		/* free global handle */
		free_global_handle(global_idx);
	}
	t->fds[fd] = -1;
	return 0;
}

int vfs_write(int fd, const void *buf, size_t len) {
	if (buf == NULL)
		return -1;
	if (fd == 1 || fd == 2) {
		/* print in chunks to avoid very large stack usage */
		const char *p = (const char *)buf;
		size_t remaining = len;
		while (remaining > 0) {
			size_t chunk = remaining > 1024 ? 1024 : remaining;
			char tmp[1025];
			for (size_t i = 0; i < chunk; ++i)
				tmp[i] = p[len - remaining + i];
			tmp[chunk] = '\0';
			printk("%s", tmp);
			remaining -= chunk;
		}
		return (int)len;
	}
	task_t *t = task_current();
	if (!t)
		return -1;
	if (fd >= 3 && fd < 32) {
		int global_idx = t->fds[fd];
		if (global_idx < 0 || global_idx >= MAX_OPEN_FILES)
			return -1;
		struct vfs_file *vf = open_files[global_idx];
		if (!vf || !active_backend || !active_backend->write_file)
			return -1;
		/* Call backend write (overwrite) */
		int r = active_backend->write_file(active_sb, vf->path, buf,
						   len);
		if (r == 0) {
			/* update cached buffer */
			if (vf->buf)
				kfree(vf->buf);
			vf->buf = (uint8_t *)kmalloc(len);
			if (vf->buf)
				for (size_t i = 0; i < len; ++i)
					vf->buf[i] = ((const uint8_t *)buf)[i];
			vf->buf_size = (uint32_t)len;
			vf->offset = (uint32_t)len;
			return (int)len;
		}
		return -1;
	}
	return -1; /* not implemented */
}

int vfs_read(int fd, void *buf, size_t len) {
	if (buf == NULL)
		return -1;
	if (fd == 0) {
		char *out = (char *)buf;
		size_t i;
		for (i = 0; i < len; ++i) {
			char c = keyboard_getchar();
			out[i] = c;
			if (c == '\n') {
				i++;
				break;
			}
		}
		return (int)i;
	}
	/* handle opened files via global handle table */
	task_t *t = task_current();
	if (!t)
		return -1;
	if (fd >= 3 && fd < 32) {
		int global_idx = t->fds[fd];
		if (global_idx < 0 || global_idx >= MAX_OPEN_FILES)
			return -1;
		struct vfs_file *vf = open_files[global_idx];
		if (!vf)
			return -1;

		/* Lazy load: read entire file on first access and cache it */
		if (!vf->buf) {
			/* Get file size if not known */
			if (vf->buf_size == 0 && active_backend &&
			    active_backend->get_file_size) {
				uint32_t sz = 0;
				int gret = active_backend->get_file_size(
					active_sb, vf->path, &sz);
				if (gret == 0 && sz > 0) {
					vf->buf_size = sz;
				}
			}

			/* Read entire file into buffer */
			if (vf->buf_size > 0 && active_backend &&
			    active_backend->read_file) {
				/* No extra padding needed - kmalloc handles canary internally */
				uint32_t alloc_size = vf->buf_size;

				vf->buf = (uint8_t *)kmalloc(alloc_size);
				if (!vf->buf) {
					printk("vfs: failed to allocate %u bytes for '%s'\n",
					       alloc_size, vf->path);
					return -1;
				}

				/* Check canary before read */
				uint32_t wanted = (alloc_size + 7) & ~7;
				uint32_t wanted_with_canary = (wanted + 4 + 7) &
							      ~7;
				uint32_t *canary_before =
					(uint32_t *)((uintptr_t)vf->buf +
						     wanted_with_canary - 4);
				uint32_t canary_val_before = *canary_before;

				vf->buf_allocated = alloc_size;
				size_t out_len = 0;
				/* Pass file size as max read length */
				int rret = active_backend->read_file(
					active_sb, vf->path, vf->buf,
					vf->buf_size, &out_len);

				/* Check canary after read */
				uint32_t canary_val_after = *canary_before;

				if (canary_val_after != canary_val_before) {
					printk("vfs_read: CANARY WAS MODIFIED! before=0x%08x after=0x%08x\n",
					       canary_val_before,
					       canary_val_after);
				}

				if (rret != 0) {
					printk("vfs: read_file failed for '%s'\n",
					       vf->path);
					kfree(vf->buf);
					vf->buf = NULL;
					return -1;
				}
				/* Verify that read didn't exceed allocated size */
				if (out_len > alloc_size) {
					printk("vfs: BUFFER OVERFLOW! read=%u > alloc=%u for '%s'\n",
					       (unsigned)out_len, alloc_size,
					       vf->path);
					kfree(vf->buf);
					vf->buf = NULL;
					return -1;
				}
				vf->buf_size = (uint32_t)out_len;
			}
		} /* Read from cached buffer */
		if (!vf->buf)
			return 0;

		uint32_t avail = vf->buf_size > vf->offset ?
					 vf->buf_size - vf->offset :
					 0;
		uint32_t to_copy = (uint32_t)len;
		if (to_copy > avail)
			to_copy = avail;
		for (uint32_t i = 0; i < to_copy; ++i)
			((uint8_t *)buf)[i] = vf->buf[vf->offset + i];
		vf->offset += to_copy;
		return (int)to_copy;
	}
	return -1;
}

int vfs_close(int fd) {
	return vfs_fd_release_for_current(fd);
}

int vfs_open(const char *pathname, int flags, int mode) {
	(void)flags;
	(void)mode;
	if (!pathname)
		return -1;
	if (!active_backend)
		return -1;

	/* allocate vfs_file and global handle */
	struct vfs_file *vf =
		(struct vfs_file *)kmalloc(sizeof(struct vfs_file));
	if (!vf)
		return -1;
	vf->type = VFS_TYPE_GENERIC;
	vf->sb = active_sb;
	vf->buf = NULL;
	vf->buf_size = 0;
	vf->buf_allocated = 0;
	vf->offset = 0;
	int i = 0;
	for (; i < (int)sizeof(vf->path) - 1 && pathname[i]; ++i)
		vf->path[i] = pathname[i];
	vf->path[i] = '\0';

	int global_idx = allocate_global_handle(vf);
	if (global_idx < 0) {
		kfree(vf);
		return -1;
	}

	/* Don't read file immediately - lazy load on first vfs_read() */
	/* Just get the file size for vfs_lseek support */
	if (active_backend && active_backend->get_file_size) {
		uint32_t sz = 0;
		int gret =
			active_backend->get_file_size(active_sb, vf->path, &sz);
		if (gret == 0 && sz > 0) {
			vf->buf_size = sz;
		}
	}

	/* allocate per-task fd and store global index */
	task_t *t = task_current();
	if (!t) {
		free_global_handle(global_idx);
		return -1;
	}
	int local_fd = vfs_fd_alloc_for_current();
	if (local_fd < 0) {
		free_global_handle(global_idx);
		return -1;
	}
	t->fds[local_fd] = global_idx;
	return local_fd;
}

int vfs_lseek(int fd, int64_t offset, int whence) {
	task_t *t = task_current();
	if (!t)
		return -1;
	if (fd < 3 || fd >= 32)
		return -1;
	int global_idx = t->fds[fd];
	if (global_idx < 0 || global_idx >= MAX_OPEN_FILES)
		return -1;
	struct vfs_file *vf = open_files[global_idx];
	if (!vf)
		return -1;
	uint64_t newoff = 0;
	if (whence == 0) { /* SEEK_SET */
		newoff = offset;
	} else if (whence == 1) { /* SEEK_CUR */
		newoff = vf->offset + offset;
	} else if (whence == 2) { /* SEEK_END */
		newoff = (uint64_t)vf->buf_size + offset;
	} else {
		return -1;
	}
	vf->offset = (uint32_t)newoff;
	return (int64_t)vf->offset;
}

int vfs_fstat(int fd, void *buf) {
	if (buf == NULL)
		return -1;
	if (fd == 1 || fd == 2 || fd == 0) {
		uint32_t mode = 0020000; /* S_IFCHR */
		(void)copy_to_user(buf, &mode, sizeof(mode));
		(void)copy_to_user((void *)((uintptr_t)buf + 16), &mode,
				   sizeof(mode));
		uint64_t z64 = 0;
		(void)copy_to_user((void *)((uintptr_t)buf + 48), &z64,
				   sizeof(z64));
		(void)copy_to_user((void *)((uintptr_t)buf + 40), &z64,
				   sizeof(z64));
		return 0;
	}
	task_t *t = task_current();
	if (!t)
		return -1;
	if (fd >= 3 && fd < 32) {
		int global_idx = t->fds[fd];
		if (global_idx < 0 || global_idx >= MAX_OPEN_FILES)
			return -1;
		struct vfs_file *vf = open_files[global_idx];
		if (!vf)
			return -1;
		uint32_t mode = 0100000; /* regular */
		(void)copy_to_user(buf, &mode, sizeof(mode));
		(void)copy_to_user((void *)((uintptr_t)buf + 16), &mode,
				   sizeof(mode));
		uint64_t sz = (uint64_t)vf->buf_size;
		(void)copy_to_user((void *)((uintptr_t)buf + 48), &sz,
				   sizeof(sz));
		(void)copy_to_user((void *)((uintptr_t)buf + 40), &sz,
				   sizeof(sz));
		return 0;
	}
	return -1;
}

int vfs_isatty(int fd) {
	return (fd == 0 || fd == 1 || fd == 2) ? 1 : 0;
}

int vfs_read_file_all(const char *path, void **out_buf, uint32_t *out_size) {
	if (!active_backend || !active_backend->get_file_size ||
	    !active_backend->read_file)
		return -1;
	if (!path || !out_buf || !out_size)
		return -1;
	uint32_t sz = 0;
	void *buf = NULL;
	size_t out_len = 0;
	/* Retry get_file_size/read_file a few times to work around transient
	 * backend read failures (e.g. block cache / ATA hiccups). */
	for (int attempt = 0; attempt < 3; ++attempt) {
		int gret = active_backend->get_file_size(active_sb, path, &sz);

		if (gret != 0)
			continue;
		if (sz == 0) {
			*out_buf = NULL;
			*out_size = 0;
			return 0;
		}
		{
			uint32_t heap_free = heap_free_bytes();
			uint32_t heap_largest = heap_largest_free_block();
			/* Allocate extra space aligned to page boundary for safety */
			uint32_t alloc_size = ((sz + 4095) / 4096) * 4096;
		}
		/* Allocate page-aligned buffer to avoid memory corruption issues */
		uint32_t alloc_size = ((sz + 4095) / 4096) * 4096;
		buf = (void *)kmalloc((size_t)alloc_size);
		if (!buf) {
			uint32_t heap_free = heap_free_bytes();
			uint32_t heap_largest = heap_largest_free_block();
			printk("vfs: kmalloc(%u) FAILED. heap_free=%u largest=%u\n",
			       (uint32_t)sz + 64, heap_free, heap_largest);
			return -3;
		}
		out_len = 0;
		int rret = active_backend->read_file(active_sb, path, buf, sz,
						     &out_len);
		if (rret == 0) {
			((uint8_t *)buf)[out_len] = '\0';
			*out_buf = buf;
			*out_size = (uint32_t)out_len;
			return 0;
		}
		kfree(buf);
		buf = NULL;
	}
	return -4;
}

int vfs_list_root(void) {
	if (!active_backend)
		return -1;

	if (active_backend->read_file == fat16_read_wrapper) {
		struct fat16_super *s = (struct fat16_super *)active_sb;
		if (!s)
			return -1;
		return fat16_list_root(s);
	} else if (active_backend->read_file == ext2_read_wrapper) {
		/* ext2: read inode 2 and call ext2_list_dir */
		struct ext2_super *s = (struct ext2_super *)active_sb;
		if (!s)
			return -1;
		struct ext2_inode inode;
		if (ext2_read_inode(s, 2, &inode) != 0)
			return -1;
		return ext2_list_dir(s, &inode);
	}

	return -1;
}

/* List a given absolute path. Delegates to backend-specific directory listing.
 * Returns 0 on success, negative on error.
 */
int vfs_list_path(const char *path) {
	if (!active_backend || !path)
		return -1;

	/* FAT16 backend */
	if (active_backend->read_file == fat16_read_wrapper) {
		struct fat16_super *s = (struct fat16_super *)active_sb;
		if (!s)
			return -1;
		return fat16_list_dir(s, path);
	} else if (active_backend->read_file == ext2_read_wrapper) {
		/* ext2: resolve path to inode and call ext2_list_dir on it */
		struct ext2_super *s = (struct ext2_super *)active_sb;
		if (!s)
			return -1;
		uint32_t inode_num;
		if (ext2_resolve_path(s, path, &inode_num) != 0)
			return -1;
		struct ext2_inode inode;
		if (ext2_read_inode(s, inode_num, &inode) != 0)
			return -1;
		/* ensure it's a directory */
		if (!(inode.i_mode & EXT2_S_IFDIR))
			return -1;
		return ext2_list_dir(s, &inode);
	}
	return -1;
}

int vfs_resolve_path(const char *path, int *is_dir, uint32_t *out_size) {
	if (!active_backend || !path)
		return -1;

	if (active_backend->read_file == fat16_read_wrapper) {
		struct fat16_super *s = (struct fat16_super *)active_sb;
		if (!s)
			return -1;
		int dir = fat16_is_dir(s, path);
		if (is_dir)
			*is_dir = dir;
		if (!dir && out_size) {
			uint32_t sz = 0;
			if (fat16_get_file_size(s, path, &sz) == 0)
				*out_size = sz;
			else
				*out_size = 0;
		}
		return dir >= 0 ? 0 : -1;
	} else if (active_backend->read_file == ext2_read_wrapper) {
		struct ext2_super *s = (struct ext2_super *)active_sb;
		if (!s)
			return -1;
		uint32_t inode_num;
		if (ext2_resolve_path(s, path, &inode_num) != 0)
			return -1;
		struct ext2_inode inode;
		if (ext2_read_inode(s, inode_num, &inode) != 0)
			return -1;
		int dir = (inode.i_mode & EXT2_S_IFDIR) ? 1 : 0;
		if (is_dir)
			*is_dir = dir;
		if (!dir && out_size)
			*out_size = inode.i_size;
		return 0;
	}
	return -1;
}
