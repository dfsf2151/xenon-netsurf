/* Forced-include compatibility shims for the xsdk/newlib toolchain. */
#ifndef XENON_COMPAT_H
#define XENON_COMPAT_H

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct in_addr;
int inet_aton(const char *cp, struct in_addr *inp);

#endif
