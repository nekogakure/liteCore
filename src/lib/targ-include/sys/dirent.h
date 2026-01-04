/* Minimal sys/dirent.h for liteCore target */
#ifndef _SYS_DIRENT_H
#define _SYS_DIRENT_H

#include <sys/types.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

struct dirent {
	ino_t d_ino;
	off_t d_off;
	unsigned short d_reclen;
	char d_name[NAME_MAX + 1];
};

/* Opaque DIR type used by newlib's dirent.h wrappers */
typedef struct __dirstream DIR;

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DIRENT_H */
