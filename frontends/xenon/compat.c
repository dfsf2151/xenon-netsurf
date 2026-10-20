/*
 * POSIX odds and ends newlib/xsdk do not implement, plus regex stubs.
 * Everything here is only as correct as NetSurf needs it to be.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int fstatat(int fd, const char *path, struct stat *buf, int flags)
{
	(void)fd;
	(void)flags;
	return stat(path, buf);
}

int access(const char *path, int mode)
{
	struct stat st;
	(void)mode;
	return stat(path, &st);
}

int rmdir(const char *path)
{
	(void)path;
	errno = ENOSYS;
	return -1;
}

char *realpath(const char *path, char *resolved_path)
{
	size_t len;

	if (path == NULL) {
		errno = EINVAL;
		return NULL;
	}
	len = strlen(path) + 1;

	if (resolved_path == NULL) {
		resolved_path = malloc(len);
		if (resolved_path == NULL) {
			errno = ENOMEM;
			return NULL;
		}
	}
	memcpy(resolved_path, path, len);
	return resolved_path;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	off_t saved = lseek(fd, 0, SEEK_CUR);
	ssize_t res;

	if (saved == (off_t)-1)
		return -1;
	if (lseek(fd, offset, SEEK_SET) == (off_t)-1)
		return -1;
	res = write(fd, buf, count);
	lseek(fd, saved, SEEK_SET);
	return res;
}

int regcomp(regex_t *preg, const char *regex, int cflags)
{
	(void)preg;
	(void)regex;
	(void)cflags;
	return REG_BADPAT;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch,
	    regmatch_t pmatch[], int eflags)
{
	(void)preg;
	(void)string;
	(void)nmatch;
	(void)pmatch;
	(void)eflags;
	return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
	(void)errcode;
	(void)preg;
	if (errbuf != NULL && errbuf_size > 0)
		errbuf[0] = '\0';
	return 0;
}

void regfree(regex_t *preg)
{
	(void)preg;
}

int scandir(const char *dirp, struct dirent ***namelist,
	    int (*filter)(const struct dirent *),
	    int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *dir;
	struct dirent *entry;
	struct dirent **list;
	size_t count = 0;
	size_t capacity = 16;

	dir = opendir(dirp);
	if (dir == NULL)
		return -1;

	list = malloc(capacity * sizeof(*list));
	if (list == NULL) {
		closedir(dir);
		errno = ENOMEM;
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		struct dirent *copy;
		if (filter != NULL && filter(entry) == 0)
			continue;
		copy = malloc(sizeof(*copy));
		if (copy == NULL)
			break;
		memcpy(copy, entry, sizeof(*copy));
		if (count == capacity) {
			struct dirent **grown;
			capacity *= 2;
			grown = realloc(list, capacity * sizeof(*list));
			if (grown == NULL) {
				free(copy);
				break;
			}
			list = grown;
		}
		list[count++] = copy;
	}
	closedir(dir);

	if (compar != NULL && count > 1) {
		qsort(list, count, sizeof(*list),
		      (int (*)(const void *, const void *))compar);
	}
	*namelist = list;
	return (int)count;
}

int ftruncate(int fd, off_t length)
{
	(void)fd;
	(void)length;
	errno = ENOSYS;
	return -1;
}
