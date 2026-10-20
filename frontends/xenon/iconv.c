/*
 * Minimal iconv for the Xbox 360 (newlib has the header but no
 * implementation). Only the encodings a browser actually meets on the
 * wire are supported: UTF-8, ASCII, ISO-8859-1, Windows-1252 and UTF-16,
 * in either direction (pages are decoded to UTF-8; form data is encoded
 * from it). "//TRANSLIT" turns characters the target lacks into '?'.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <errno.h>
#include <stdbool.h>
#include <iconv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/utsname.h>

enum xenon_encoding {
	ENC_UNKNOWN = 0,
	ENC_UTF8,
	ENC_ASCII,
	ENC_LATIN1,
	ENC_CP1252,
	ENC_UTF16LE,
	ENC_UTF16BE,
};

struct xenon_iconv {
	enum xenon_encoding from;
	enum xenon_encoding to;
	bool translit;
};

static const uint16_t cp1252_high[32] = {
	0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
	0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static enum xenon_encoding encoding_from_name(const char *name)
{
	char buf[32];
	size_t i;

	if (name == NULL)
		return ENC_UNKNOWN;

	for (i = 0; i < sizeof(buf) - 1 && name[i] != '\0' && name[i] != '/'; i++) {
		char c = name[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		buf[i] = c;
	}
	buf[i] = '\0';

	if (strcmp(buf, "utf-8") == 0 || strcmp(buf, "utf8") == 0)
		return ENC_UTF8;
	if (strcmp(buf, "us-ascii") == 0 || strcmp(buf, "ascii") == 0)
		return ENC_ASCII;
	if (strcmp(buf, "iso-8859-1") == 0 || strcmp(buf, "latin1") == 0 ||
	    strcmp(buf, "iso8859-1") == 0 || strcmp(buf, "l1") == 0)
		return ENC_LATIN1;
	if (strcmp(buf, "windows-1252") == 0 || strcmp(buf, "cp1252") == 0)
		return ENC_CP1252;
	if (strcmp(buf, "utf-16le") == 0 || strcmp(buf, "ucs-2le") == 0)
		return ENC_UTF16LE;
	if (strcmp(buf, "utf-16be") == 0 || strcmp(buf, "ucs-2be") == 0)
		return ENC_UTF16BE;

	return ENC_UNKNOWN;
}

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
	struct xenon_iconv *cd;

	if (tocode == NULL || fromcode == NULL) {
		errno = EINVAL;
		return (iconv_t)-1;
	}

	cd = malloc(sizeof(*cd));
	if (cd == NULL) {
		errno = ENOMEM;
		return (iconv_t)-1;
	}

	cd->from = encoding_from_name(fromcode);
	cd->to = encoding_from_name(tocode);
	cd->translit = strstr(tocode, "//TRANSLIT") != NULL ||
		       strstr(tocode, "//translit") != NULL;

	if (cd->from == ENC_UNKNOWN || cd->to == ENC_UNKNOWN) {
		free(cd);
		errno = EINVAL;
		return (iconv_t)-1;
	}

	return (iconv_t)cd;
}

static size_t put_utf8(uint32_t cp, char *out, size_t room)
{
	if (cp < 0x80) {
		if (room < 1)
			return 0;
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		if (room < 2)
			return 0;
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		if (room < 3)
			return 0;
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	if (room < 4)
		return 0;
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

/* One code point in the target encoding: the bytes written, 0 when out of
 * room (E2BIG), or (size_t)-1 when the target can't hold it (EILSEQ). */
static size_t put_char(const struct xenon_iconv *cd, uint32_t cp, char *out,
		       size_t room)
{
	switch (cd->to) {
	case ENC_UTF8:
		return put_utf8(cp, out, room);
	case ENC_UTF16LE:
	case ENC_UTF16BE: {
		uint16_t units[2];
		size_t n = 1;
		if (cp >= 0x10000) {
			cp -= 0x10000;
			units[0] = (uint16_t)(0xd800 | (cp >> 10));
			units[1] = (uint16_t)(0xdc00 | (cp & 0x3ff));
			n = 2;
		} else {
			units[0] = (uint16_t)cp;
		}
		if (room < n * 2)
			return 0;
		for (size_t i = 0; i < n; i++) {
			if (cd->to == ENC_UTF16LE) {
				out[i * 2] = (char)(units[i] & 0xff);
				out[i * 2 + 1] = (char)(units[i] >> 8);
			} else {
				out[i * 2] = (char)(units[i] >> 8);
				out[i * 2 + 1] = (char)(units[i] & 0xff);
			}
		}
		return n * 2;
	}
	default: {
		/* Single-byte targets. */
		int byte = -1;
		if (cp < 0x80)
			byte = (int)cp;
		else if (cd->to == ENC_LATIN1 && cp < 0x100)
			byte = (int)cp;
		else if (cd->to == ENC_CP1252) {
			if (cp >= 0xa0 && cp < 0x100)
				byte = (int)cp;
			for (int i = 0; byte < 0 && i < 32; i++)
				if (cp1252_high[i] == cp)
					byte = 0x80 + i;
		}
		if (byte < 0) {
			if (!cd->translit)
				return (size_t)-1;
			byte = '?';
		}
		if (room < 1)
			return 0;
		out[0] = (char)byte;
		return 1;
	}
	}
}

size_t iconv(iconv_t cdp, char **inbuf, size_t *inbytesleft,
	     char **outbuf, size_t *outbytesleft)
{
	struct xenon_iconv *cd = (struct xenon_iconv *)cdp;
	char *in;
	char *out;
	size_t inleft;
	size_t outleft;

	if (cd == NULL) {
		errno = EBADF;
		return (size_t)-1;
	}
	if (inbuf == NULL || *inbuf == NULL) {
		return 0;
	}

	in = *inbuf;
	out = *outbuf;
	inleft = *inbytesleft;
	outleft = *outbytesleft;

	while (inleft > 0) {
		uint32_t cp;
		size_t step;
		size_t produced;

		if (cd->from == ENC_UTF8) {
			const unsigned char c = (unsigned char)in[0];
			if (c < 0x80) {
				cp = c;
				step = 1;
			} else if (c >= 0xf0 && inleft >= 4) {
				cp = ((uint32_t)(c & 0x07) << 18) | ((in[1] & 0x3f) << 12) |
				     ((in[2] & 0x3f) << 6) | (in[3] & 0x3f);
				step = 4;
			} else if (c >= 0xe0 && inleft >= 3) {
				cp = ((uint32_t)(c & 0x0f) << 12) | ((in[1] & 0x3f) << 6) |
				     (in[2] & 0x3f);
				step = 3;
			} else if (c >= 0xc0 && inleft >= 2) {
				cp = ((uint32_t)(c & 0x1f) << 6) | (in[1] & 0x3f);
				step = 2;
			} else {
				errno = EILSEQ;
				goto out;
			}
		} else if (cd->from == ENC_ASCII || cd->from == ENC_LATIN1) {
			cp = (unsigned char)in[0];
			step = 1;
		} else if (cd->from == ENC_CP1252) {
			const unsigned char c = (unsigned char)in[0];
			cp = (c >= 0x80 && c <= 0x9f) ? cp1252_high[c - 0x80] : c;
			step = 1;
		} else {
			if (inleft < 2) {
				errno = EINVAL;
				goto out;
			}
			if (cd->from == ENC_UTF16LE)
				cp = (unsigned char)in[0] | ((uint32_t)(unsigned char)in[1] << 8);
			else
				cp = ((uint32_t)(unsigned char)in[0] << 8) | (unsigned char)in[1];
			step = 2;
		}

		produced = put_char(cd, cp, out, outleft);
		if (produced == (size_t)-1) {
			errno = EILSEQ;
			goto out;
		}
		if (produced == 0) {
			errno = E2BIG;
			goto out;
		}
		in += step;
		inleft -= step;
		out += produced;
		outleft -= produced;
	}

out:
	*inbuf = in;
	*inbytesleft = inleft;
	*outbuf = out;
	*outbytesleft = outleft;

	if (inleft > 0) {
		if (errno != E2BIG && errno != EINVAL)
			errno = EILSEQ;
		return (size_t)-1;
	}
	return 0;
}

int iconv_close(iconv_t cdp)
{
	free(cdp);
	return 0;
}

/* newlib/xsdk provide inet_pton() but not the legacy inet_aton(). */
int inet_aton(const char *cp, struct in_addr *inp)
{
	return inet_pton(AF_INET, cp, inp) == 1;
}

int uname(struct utsname *buf)
{
	if (buf == NULL)
		return -1;
	memset(buf, 0, sizeof(*buf));
	strcpy(buf->sysname, "XenonOS");
	strcpy(buf->nodename, "xbox360");
	strcpy(buf->release, "1.0");
	strcpy(buf->version, "xsdk");
	strcpy(buf->machine, "Xbox 360");
	return 0;
}
