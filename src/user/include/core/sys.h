#ifndef CORE_SYS_H
#define CORE_SYS_H

#include <stddef.h>
#include <sys/types.h>

int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int listdir(const char *path);

ssize_t write(int fd, const void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);
int open(const char *pathname, int flags, int mode);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int fstat(int fd, void *st);

void *_sbrk(ptrdiff_t increment);

#endif /* CORE_SYS_H */
