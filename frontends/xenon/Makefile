#
# Makefile for NetSurf xenon (Xbox 360) target
#
# This file is part of NetSurf

# ----------------------------------------------------------------------------
# Xenon flag setup
# ----------------------------------------------------------------------------

CFLAGS += -std=gnu17 -Dxenon -Dnsxenon -g -I$(FRONTEND_SOURCE_DIR)/include -include $(FRONTEND_SOURCE_DIR)/include/xenon_compat.h

# Repo root: netsurf-src/frontends/xenon -> ../..
XENON_ROOT ?= $(abspath $(FRONTEND_SOURCE_DIR)/../../..)

# The mbedTLS port for xsdk (third_party/mbedtls-xenon), built with its
# cmake xsdk toolchain into build-xsdk.
XENON_MBEDTLS ?= $(XENON_ROOT)/third_party/mbedtls-xenon

CFLAGS += -I$(XENON_MBEDTLS)/include
# The library was built with this user config (TLS 1.3 off, settable clock);
# compiling against a different one would change the ABI.
CFLAGS += -DMBEDTLS_USER_CONFIG_FILE='"$(XENON_MBEDTLS)/configs/config-xenon-netsurf.h"'

LDFLAGS += -lm -lz \
	   -L$(XENON_MBEDTLS)/build-xsdk/library -lmbedtls -lmbedx509 -lmbedcrypto

# ---------------------------------------------------------------------------
# Target setup
# ---------------------------------------------------------------------------

XENON_RESOURCES_DIR := $(FRONTEND_RESOURCES_DIR)
XENON_MESSAGES := en

MESSAGES_LANGUAGES := en
MESSAGES_TARGET := resources

# ---------------------------------------------------------------------------
# HOST specific feature flags
# ---------------------------------------------------------------------------

CFLAGS += -D_POSIX_C_SOURCE=200809L \
	  -D_XOPEN_SOURCE=700 \
	  -D_BSD_SOURCE \
	  -D_DEFAULT_SOURCE

# ----------------------------------------------------------------------------
# Source file setup
# ----------------------------------------------------------------------------

S_FRONTEND := main.c output.c filetype.c schedule.c bitmap.c plot.c browser.c \
	download.c 401login.c layout.c dispatch.c fetch.c iconv.c compat.c font8x16.c font.c draw.c \
	http.c input.c ui.c time.c audio.c minimp3_impl.c

# This is the final source build list
SOURCES = $(S_COMMON) $(S_IMAGE) $(S_BROWSER) $(S_PDF) $(S_FRONTEND)
EXETARGET := nsxenon

# ----------------------------------------------------------------------------
# Xbox 360 / xsdk link
# ----------------------------------------------------------------------------

XSDK_ROOT ?= $(XENON_ROOT)/../UCN_MVP_X360/third_party/xsdk
XENON_PREFIX ?= $(XENON_ROOT)/prefix

LDFLAGS += -L$(XENON_PREFIX)/lib \
	-static -nostdlib -T $(XSDK_ROOT)/src/xsdk.ld \
	-Wl,--start-group \
	$(XSDK_ROOT)/build/libxsdk.a \
	-lcss -ldom -lhubbub -lparserutils -lwapcaplet -lnsutils -lutf8proc \
	-lstdc++ -lc -lm -lgcc \
	-Wl,--end-group

# ----------------------------------------------------------------------------
# Install target
# ----------------------------------------------------------------------------

install-xenon:

# ----------------------------------------------------------------------------
# Package target
# ----------------------------------------------------------------------------

package-xenon:
