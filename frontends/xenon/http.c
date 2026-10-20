/*
 * HTTP and HTTPS fetcher for the xenon (Xbox 360) frontend, on xsdk's
 * sockets and the mbedTLS port in third_party/mbedtls-xenon.
 *
 * One request at a time per fetch context: connect, GET, parse the status
 * line and headers, then stream the body to the fetch core. Redirects go
 * back to llcache with FETCH_REDIRECT; chunked bodies are decoded. A
 * connection is kept alive for the next request to the same host, which
 * takes the TLS handshake out of a page's later objects. Certificates are
 * checked against cacert.pem beside the xex, using a clock corrected from
 * the Date headers.
 *
 * Sockets are left blocking: XAM's select in the emulator only reports
 * readiness when its timeout expires, so a poll on the console should
 * replace this once the fetcher must stay responsive to a dead server.
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 * Licensed under the GNU General Public License, version 2.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libwapcaplet/libwapcaplet.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/nsurl.h"
#include "utils/ring.h"

#include "content/fetch.h"
#include "content/fetchers.h"

#include "xenon/http.h"
#include "xenon/time.h"

#define HTTP_PORT "80"
#define HTTPS_PORT "443"
#define HTTP_BUF_SIZE 4096
#define HTTP_LINE_MAX 1024

struct fetch_http_context {
	struct fetch_http_context *r_next, *r_prev;

	struct fetch *fetchh;
	nsurl *url;

	bool only_2xx;
	bool aborted;
	bool locked;
};

static struct fetch_http_context *ring;

/* ------------------------------------------------------------------ */
/* socket helpers                                                     */
/* ------------------------------------------------------------------ */

/* A hosts.txt next to the xex pins names to addresses. The emulator's
 * resolver is not always there, and a fixed address makes network tests
 * repeatable. Lines are "name address" or "address name"; # comments. */
struct host_entry {
	char name[128];
	struct in_addr address;
};

static struct host_entry hosts[32];
static int host_count = -1;

static void hosts_load(void)
{
	char line[256];
	FILE *fp;

	host_count = 0;
	fp = fopen("hosts.txt", "r");
	if (fp == NULL)
		return;

	while (host_count < (int)(sizeof(hosts) / sizeof(hosts[0])) &&
	       fgets(line, sizeof(line), fp) != NULL) {
		char *first;
		char *second;
		char *hash = strchr(line, '#');
		struct in_addr address;

		if (hash != NULL)
			*hash = '\0';

		first = strtok(line, " \t\r\n");
		second = strtok(NULL, " \t\r\n");
		if (first == NULL || second == NULL)
			continue;

		if (inet_pton(AF_INET, first, &address) == 1) {
			snprintf(hosts[host_count].name,
				 sizeof(hosts[host_count].name), "%s", second);
		} else if (inet_pton(AF_INET, second, &address) == 1) {
			snprintf(hosts[host_count].name,
				 sizeof(hosts[host_count].name), "%s", first);
		} else {
			continue;
		}

		hosts[host_count].address = address;
		host_count++;
	}

	fclose(fp);
	NSLOG(netsurf, INFO, "http: %d pinned host(s) from hosts.txt", host_count);
}

static bool hosts_lookup(const char *host, struct in_addr *address)
{
	if (host_count < 0)
		hosts_load();

	for (int i = 0; i < host_count; i++) {
		if (strcasecmp(hosts[i].name, host) == 0) {
			*address = hosts[i].address;
			return true;
		}
	}
	return false;
}

static int http_connect(const char *host, const char *port)
{
	struct addrinfo hints;
	struct addrinfo *list = NULL;
	struct addrinfo *entry;
	struct in_addr fixed;
	int fd = -1;

	if (hosts_lookup(host, &fixed)) {
		struct sockaddr_in address;

		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons((uint16_t)atoi(port));
		address.sin_addr = fixed;

		fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (fd >= 0 &&
		    connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
			return fd;
		if (fd >= 0)
			close(fd);
		NSLOG(netsurf, INFO, "http: cannot connect %s:%s (pinned)",
		      host, port);
		return -1;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(host, port, &hints, &list) != 0) {
		NSLOG(netsurf, INFO, "http: cannot resolve %s", host);
		return -1;
	}

	for (entry = list; entry != NULL; entry = entry->ai_next) {
		fd = socket(entry->ai_family, entry->ai_socktype,
			    entry->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}

	freeaddrinfo(list);

	if (fd < 0)
		NSLOG(netsurf, INFO, "http: cannot connect %s:%s", host, port);

	return fd;
}

static bool http_send_all(int fd, const char *data, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t res = send(fd, data + done, len - done, 0);

		if (res <= 0) {
			if (res < 0 && errno == EINTR)
				continue;
			return false;
		}
		done += (size_t)res;
	}

	return true;
}

/* ------------------------------------------------------------------ */
/* TLS transport                                                      */
/* ------------------------------------------------------------------ */

struct http_conn {
	int fd;
	bool tls;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
};

static mbedtls_x509_crt ca_chain;
static mbedtls_ctr_drbg_context ctr_drbg;
static mbedtls_entropy_context entropy;
static int tls_state; /* 0 untried, 1 ready, -1 failed */

static void tls_log_error(const char *what, int code)
{
	char text[128];

	mbedtls_strerror(code, text, sizeof(text));
	NSLOG(netsurf, INFO, "tls: %s: %s (-0x%04x)", what, text, -code);
}

static bool tls_init(void)
{
	int ret;

	if (tls_state == 1)
		return true;
	if (tls_state == -1)
		return false;

	mbedtls_x509_crt_init(&ca_chain);
	mbedtls_ctr_drbg_init(&ctr_drbg);
	mbedtls_entropy_init(&entropy);

	ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func,
				    &entropy,
				    (const unsigned char *)"xenon-netsurf",
				    sizeof("xenon-netsurf") - 1);
	if (ret != 0) {
		tls_log_error("cannot seed the random generator", ret);
		tls_state = -1;
		return false;
	}

	/* The CA bundle sits beside the xex, like the resources do. */
	ret = mbedtls_x509_crt_parse_file(&ca_chain, "cacert.pem");
	if (ret != 0) {
		NSLOG(netsurf, INFO, "tls: cannot read cacert.pem (error %d)",
		      ret);
		tls_state = -1;
		return false;
	}

	tls_state = 1;
	return true;
}

static int tls_send(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t res = send(fd, buf, len, 0);

	if (res < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
	return (int)res;
}

static int tls_recv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t res = recv(fd, buf, len, 0);

	if (res < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	return (int)res;
}

static void http_conn_free(struct http_conn *conn)
{
	if (conn->tls) {
		(void)mbedtls_ssl_close_notify(&conn->ssl);
		mbedtls_ssl_free(&conn->ssl);
		mbedtls_ssl_config_free(&conn->conf);
	}
	if (conn->fd >= 0)
		close(conn->fd);
	free(conn);
}

/* Plain HTTP, headers only. Called once when the certificate looks
 * expired because the console has never had its clock set: port 80 still
 * answers with a Date header even when the page redirects to HTTPS. */
static bool http_probe_date(const char *host)
{
	char request[512];
	char buffer[1024];
	size_t used = 0;
	char *line;
	char *end;
	int fd;

	fd = http_connect(host, HTTP_PORT);
	if (fd < 0)
		return false;

	snprintf(request, sizeof(request),
		 "HEAD / HTTP/1.0\r\n"
		 "Host: %s\r\n"
		 "Connection: close\r\n"
		 "\r\n", host);

	if (!http_send_all(fd, request, strlen(request))) {
		close(fd);
		return false;
	}

	while (used + 1 < sizeof(buffer)) {
		ssize_t got = recv(fd, buffer + used, sizeof(buffer) - 1 - used, 0);

		if (got <= 0)
			break;
		used += (size_t)got;
		buffer[used] = '\0';
		if (strstr(buffer, "\r\n\r\n") != NULL)
			break;
	}
	close(fd);

	line = strstr(buffer, "\r\nDate:");
	if (line == NULL)
		return false;
	line += sizeof("\r\nDate:") - 1;
	while (*line == ' ')
		line++;
	end = strstr(line, "\r\n");
	if (end != NULL)
		*end = '\0';

	xenon_time_note_http_date(line);
	return xenon_time_has_anchor();
}

static struct http_conn *http_conn_open_inner(const char *host,
		const char *port, bool tls, const char **why, bool may_probe);

static struct http_conn *http_conn_open(const char *host, const char *port,
					bool tls, const char **why)
{
	return http_conn_open_inner(host, port, tls, why, true);
}

static struct http_conn *http_conn_open_inner(const char *host,
		const char *port, bool tls, const char **why, bool may_probe)
{
	struct http_conn *conn;
	int ret;

	*why = "Cannot reach server";
	conn = calloc(1, sizeof(*conn));
	if (conn == NULL) {
		*why = "Out of memory";
		return NULL;
	}

	conn->fd = http_connect(host, port);
	if (conn->fd < 0) {
		free(conn);
		return NULL;
	}
	if (!tls) {
		NSLOG(fetch, DEBUG, "http: connected to %s:%s", host, port);
		return conn;
	}

	if (!tls_init()) {
		*why = "TLS is not available (cacert.pem?)";
		http_conn_free(conn);
		return NULL;
	}

	conn->tls = true;
	mbedtls_ssl_init(&conn->ssl);
	mbedtls_ssl_config_init(&conn->conf);

	ret = mbedtls_ssl_config_defaults(&conn->conf, MBEDTLS_SSL_IS_CLIENT,
					  MBEDTLS_SSL_TRANSPORT_STREAM,
					  MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret != 0)
		goto fail;
	mbedtls_ssl_conf_authmode(&conn->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&conn->conf, &ca_chain, NULL);
	mbedtls_ssl_conf_rng(&conn->conf, mbedtls_ctr_drbg_random, &ctr_drbg);
	mbedtls_ssl_conf_min_tls_version(&conn->conf,
					 MBEDTLS_SSL_VERSION_TLS1_2);

	ret = mbedtls_ssl_setup(&conn->ssl, &conn->conf);
	if (ret != 0)
		goto fail;
	ret = mbedtls_ssl_set_hostname(&conn->ssl, host);
	if (ret != 0)
		goto fail;
	mbedtls_ssl_set_bio(&conn->ssl, &conn->fd, tls_send, tls_recv, NULL);

	while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		tls_log_error(host, ret);
		if (may_probe && !xenon_time_has_anchor() &&
		    http_probe_date(host)) {
			NSLOG(netsurf, INFO, "tls: retrying %s with the "
			      "server's clock", host);
			http_conn_free(conn);
			return http_conn_open_inner(host, port, tls, why, false);
		}
		*why = "TLS handshake failed";
		http_conn_free(conn);
		return NULL;
	}

	uint32_t flags = mbedtls_ssl_get_verify_result(&conn->ssl);
	if (flags != 0) {
		char info[512];

		mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
		NSLOG(netsurf, INFO, "tls: %s: %s", host, info);
		if (may_probe && !xenon_time_has_anchor() &&
		    http_probe_date(host)) {
			NSLOG(netsurf, INFO, "tls: retrying %s with the "
			      "server's clock", host);
			http_conn_free(conn);
			return http_conn_open_inner(host, port, tls, why, false);
		}
		*why = "Server certificate is not valid";
		http_conn_free(conn);
		return NULL;
	}

	NSLOG(fetch, DEBUG, "http: connected to %s:%s with TLS", host, port);
	return conn;

fail:
	tls_log_error("connection setup", ret);
	*why = "TLS setup failed";
	http_conn_free(conn);
	return NULL;
}

static ssize_t http_conn_read(struct http_conn *conn, void *buf, size_t len)
{
	if (!conn->tls)
		return recv(conn->fd, buf, len, 0);

	for (;;) {
		int ret = mbedtls_ssl_read(&conn->ssl, buf, len);

		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		return ret;
	}
}

static bool http_conn_write(struct http_conn *conn, const char *data,
			    size_t len)
{
	size_t done = 0;

	if (!conn->tls)
		return http_send_all(conn->fd, data, len);

	while (done < len) {
		int ret = mbedtls_ssl_write(&conn->ssl,
					    (const unsigned char *)data + done,
					    len - done);

		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (ret <= 0)
			return false;
		done += (size_t)ret;
	}

	return true;
}

/* ------------------------------------------------------------------ */
/* connection pool                                                    */
/* ------------------------------------------------------------------ */

/* A page alternates between a few hosts: the wiki itself for HTML and
 * CSS, thumb.wikimedia.org for the scaled images, upload.wikimedia.org for
 * the odd original. One slot per host means the TLS session is reused
 * instead of rebuilt every time the host changes. */
#define POOL_SLOTS 6

struct pool_slot {
	struct http_conn *conn;
	char host[256];
	char port[16];
	bool tls;
	unsigned last_used;
};

static struct pool_slot pool[POOL_SLOTS];
static unsigned pool_clock;
static int pool_current = -1;

/* Drop the connection the current transaction is using. */
static void pool_release(void)
{
	if (pool_current >= 0 && pool[pool_current].conn != NULL) {
		http_conn_free(pool[pool_current].conn);
		pool[pool_current].conn = NULL;
	}
	pool_current = -1;
}

static bool pool_matches(const char *host, const char *port, bool tls)
{
	for (int i = 0; i < POOL_SLOTS; i++) {
		if (pool[i].conn != NULL && pool[i].tls == tls &&
		    strcmp(pool[i].host, host) == 0 &&
		    strcmp(pool[i].port, port) == 0)
			return true;
	}
	return false;
}

/* The pooled connection for this target, or a new one. On failure *why
 * says what to show the user. */
static struct http_conn *pool_acquire(const char *host, const char *port,
				      bool tls, const char **why)
{
	struct http_conn *conn;
	int slot = -1;
	int free_slot = -1;

	pool_current = -1;

	for (int i = 0; i < POOL_SLOTS; i++) {
		if (pool[i].conn == NULL) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (pool[i].tls == tls &&
		    strcmp(pool[i].host, host) == 0 &&
		    strcmp(pool[i].port, port) == 0) {
			slot = i;
			break;
		}
	}

	if (slot >= 0) {
		pool[slot].last_used = ++pool_clock;
		pool_current = slot;
		return pool[slot].conn;
	}

	if (free_slot < 0) {
		/* Evict the least recently used idle connection. */
		slot = 0;
		for (int i = 1; i < POOL_SLOTS; i++) {
			if (pool[i].last_used < pool[slot].last_used)
				slot = i;
		}
		http_conn_free(pool[slot].conn);
		pool[slot].conn = NULL;
	} else {
		slot = free_slot;
	}

	conn = http_conn_open(host, port, tls, why);
	if (conn == NULL)
		return NULL;

	pool[slot].conn = conn;
	snprintf(pool[slot].host, sizeof(pool[slot].host), "%s", host);
	snprintf(pool[slot].port, sizeof(pool[slot].port), "%s", port);
	pool[slot].tls = tls;
	pool[slot].last_used = ++pool_clock;
	pool_current = slot;

	return conn;
}

/* ------------------------------------------------------------------ */
/* buffered reader                                                    */
/* ------------------------------------------------------------------ */

struct http_reader {
	struct http_conn *conn;
	uint8_t buf[HTTP_BUF_SIZE];
	size_t pos;
	size_t len;
	bool eof;
	bool error;
};

static void reader_init(struct http_reader *r, struct http_conn *conn)
{
	memset(r, 0, sizeof(*r));
	r->conn = conn;
}

static bool reader_fill(struct http_reader *r)
{
	ssize_t got;

	if (r->pos < r->len)
		return true;
	if (r->eof || r->error)
		return false;

	got = http_conn_read(r->conn, r->buf, sizeof(r->buf));
	if (got == 0) {
		r->eof = true;
		return false;
	}
	if (got < 0) {
		if (errno == EINTR)
			return false;
		r->error = true;
		return false;
	}

	r->pos = 0;
	r->len = (size_t)got;
	return true;
}

static int reader_getc(struct http_reader *r)
{
	if (!reader_fill(r))
		return -1;
	return r->buf[r->pos++];
}

/* Read one line, stripped of its CR/LF. */
static bool reader_line(struct http_reader *r, char *line, size_t size)
{
	size_t used = 0;
	int c;

	for (;;) {
		c = reader_getc(r);
		if (c < 0) {
			if (used > 0) {
				line[used] = '\0';
				return true;
			}
			return false;
		}
		if (c == '\n')
			break;
		if (used + 1 < size)
			line[used++] = (char)c;
	}

	while (used > 0 && (line[used - 1] == '\r' || line[used - 1] == ' '))
		used--;

	line[used] = '\0';
	return true;
}

/* Give the caller any buffered bytes (at most `size`). */
static size_t reader_take(struct http_reader *r, const uint8_t **data,
			  size_t size)
{
	size_t avail;

	if (!reader_fill(r))
		return 0;

	avail = r->len - r->pos;
	if (avail > size)
		avail = size;
	*data = r->buf + r->pos;
	r->pos += avail;
	return avail;
}

/* ------------------------------------------------------------------ */
/* fetch callbacks                                                    */
/* ------------------------------------------------------------------ */

static void http_send(struct fetch_http_context *ctx, const fetch_msg *msg)
{
	ctx->locked = true;
	fetch_send_callback(msg, ctx->fetchh);
	ctx->locked = false;
}

static bool http_send_header(struct fetch_http_context *ctx, const char *fmt,
			     ...)
{
	fetch_msg msg;
	char header[HTTP_LINE_MAX];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(header, sizeof(header), fmt, ap);
	va_end(ap);

	if (len <= 0 || (size_t)len >= sizeof(header))
		return false;

	msg.type = FETCH_HEADER;
	msg.data.header_or_data.buf = (const uint8_t *)header;
	msg.data.header_or_data.len = (size_t)len;

	http_send(ctx, &msg);

	return !ctx->aborted;
}

static bool http_send_data(struct fetch_http_context *ctx,
			   const uint8_t *data, size_t len)
{
	fetch_msg msg;

	if (ctx->aborted)
		return false;

	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = data;
	msg.data.header_or_data.len = len;

	http_send(ctx, &msg);

	return !ctx->aborted;
}

static void http_send_error(struct fetch_http_context *ctx,
			    const char *message)
{
	fetch_msg msg;

	msg.type = FETCH_ERROR;
	msg.data.error = message;
	http_send(ctx, &msg);
}

static void http_send_finished(struct fetch_http_context *ctx)
{
	fetch_msg msg;

	msg.type = FETCH_FINISHED;
	http_send(ctx, &msg);
}

static bool http_icontains(const char *haystack, const char *needle)
{
	size_t needle_len = strlen(needle);
	const char *p;

	for (p = haystack; *p != '\0'; p++) {
		if (strncasecmp(p, needle, needle_len) == 0)
			return true;
	}

	return false;
}

/* ------------------------------------------------------------------ */
/* the transaction                                                    */
/* ------------------------------------------------------------------ */

struct http_response {
	int code;
	long long content_length; /* -1 when absent */
	bool chunked;
	bool close_after;
	char content_type[256];
	char location[1024];
	char date[64];
};

static bool http_parse_status(const char *line, int *code)
{
	const char *space;

	if (strncasecmp(line, "HTTP/", 5) != 0)
		return false;

	space = strchr(line, ' ');
	if (space == NULL)
		return false;

	*code = atoi(space + 1);
	return *code >= 100 && *code < 600;
}

static bool http_read_headers(struct http_reader *r, struct http_response *resp)
{
	char line[HTTP_LINE_MAX];
	int first = 1;

	memset(resp, 0, sizeof(*resp));
	resp->content_length = -1;

	while (reader_line(r, line, sizeof(line))) {
		if (line[0] == '\0')
			return !first;

		if (first) {
			if (!http_parse_status(line, &resp->code))
				return false;
			/* HTTP/1.0 closes unless it says keep-alive. */
			resp->close_after =
				strncmp(line, "HTTP/1.0", 8) == 0;
			first = 0;
			continue;
		}

		if (strncasecmp(line, "Content-Type:", 13) == 0) {
			const char *value = line + 13;
			while (*value == ' ')
				value++;
			snprintf(resp->content_type, sizeof(resp->content_type),
				 "%s", value);
		} else if (strncasecmp(line, "Content-Length:", 15) == 0) {
			resp->content_length = strtoll(line + 15, NULL, 10);
		} else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
			if (http_icontains(line + 18, "chunked"))
				resp->chunked = true;
		} else if (strncasecmp(line, "Location:", 9) == 0) {
			const char *value = line + 9;
			while (*value == ' ')
				value++;
			snprintf(resp->location, sizeof(resp->location), "%s",
				 value);
		} else if (strncasecmp(line, "Date:", 5) == 0) {
			const char *value = line + 5;
			while (*value == ' ')
				value++;
			snprintf(resp->date, sizeof(resp->date), "%s", value);
		} else if (strncasecmp(line, "Connection:", 11) == 0) {
			if (http_icontains(line + 11, "close"))
				resp->close_after = true;
			else if (http_icontains(line + 11, "keep-alive"))
				resp->close_after = false;
		}
	}

	return false;
}

static bool http_send_body_length(struct fetch_http_context *ctx,
				  struct http_reader *r, long long length)
{
	while (length > 0) {
		const uint8_t *data;
		size_t want = length > HTTP_BUF_SIZE ?
			      HTTP_BUF_SIZE : (size_t)length;
		size_t got = reader_take(r, &data, want);

		if (got == 0)
			return false;
		if (!http_send_data(ctx, data, got))
			return false;
		length -= (long long)got;
	}

	return true;
}

static bool http_send_body_close(struct fetch_http_context *ctx,
				 struct http_reader *r)
{
	for (;;) {
		const uint8_t *data;
		size_t got = reader_take(r, &data, HTTP_BUF_SIZE);

		if (got == 0)
			return true;
		if (!http_send_data(ctx, data, got))
			return false;
	}
}

static bool http_send_body_chunked(struct fetch_http_context *ctx,
				   struct http_reader *r)
{
	char line[HTTP_LINE_MAX];

	for (;;) {
		unsigned long size;
		char *end;

		if (!reader_line(r, line, sizeof(line)))
			return false;

		size = strtoul(line, &end, 16);
		if (end == line)
			return false;

		if (size == 0) {
			/* trailers, then the final empty line */
			while (reader_line(r, line, sizeof(line)) &&
			       line[0] != '\0')
				;
			return true;
		}

		if (!http_send_body_length(ctx, r, (long long)size))
			return false;

		/* trailing CRLF */
		if (reader_getc(r) < 0)
			return false;
		if (reader_getc(r) < 0)
			return false;
	}
}

static char *http_build_target(const nsurl *url)
{
	char *path = NULL;
	size_t path_len = 0;

	if (nsurl_get(url, NSURL_PATH | NSURL_QUERY, &path, &path_len) !=
	    NSERROR_OK)
		return NULL;

	if (path_len == 0) {
		free(path);
		return strdup("/");
	}

	return path;
}

static void fetch_http_transaction(struct fetch_http_context *ctx)
{
	struct http_reader reader;
	struct http_response resp;
	struct http_conn *conn = NULL;
	lwc_string *host = NULL;
	lwc_string *port = NULL;
	bool tls = nsurl_get_scheme_type(ctx->url) == NSURL_SCHEME_HTTPS;
	const char *port_text = tls ? HTTPS_PORT : HTTP_PORT;
	const char *why = "Cannot reach server";
	char *target = NULL;
	char request[2048];
	bool ok = false;

	host = nsurl_get_component(ctx->url, NSURL_HOST);
	if (host == NULL)
		goto out;

	port = nsurl_get_component(ctx->url, NSURL_PORT);
	if (port != NULL && lwc_string_length(port) > 0)
		port_text = lwc_string_data(port);

	target = http_build_target(ctx->url);
	if (target == NULL)
		goto out;

	if (snprintf(request, sizeof(request),
		     "GET %s HTTP/1.1\r\n"
		     "Host: %s\r\n"
		     "User-Agent: NetSurf/3.11 (Xenon)\r\n"
		     "Accept: text/html,text/css,*/*;q=0.1\r\n"
		     "Accept-Encoding: identity\r\n"
		     "Connection: keep-alive\r\n"
		     "\r\n",
		     target, lwc_string_data(host)) >= (int)sizeof(request))
		goto out;

	{
		int attempt;

		for (attempt = 0; attempt < 2; attempt++) {
			bool reused = pool_matches(lwc_string_data(host),
						   port_text, tls);

			conn = pool_acquire(lwc_string_data(host),
					    port_text, tls, &why);
			if (conn == NULL) {
				http_send_error(ctx, why);
				goto out;
			}

			if (http_conn_write(conn, request, strlen(request))) {
				reader_init(&reader, conn);
				if (http_read_headers(&reader, &resp))
					break;
			}

			/* A socket kept from an earlier request may have
			 * been closed while it sat idle: retry once on a
			 * fresh one. */
			pool_release();
			conn = NULL;
			if (!reused)
				break;
		}

		if (conn == NULL) {
			http_send_error(ctx, "Connection failed");
			goto out;
		}
	}

	xenon_time_note_http_date(resp.date);

	fetch_set_http_code(ctx->fetchh, resp.code);

	/* A redirect goes back to the core, which joins relative targets. */
	if ((resp.code == 301 || resp.code == 302 || resp.code == 303 ||
	     resp.code == 307 || resp.code == 308) &&
	    resp.location[0] != '\0') {
		fetch_msg msg;

		/* Its body is unread, so the socket cannot be reused. */
		pool_release();
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = resp.location;
		http_send(ctx, &msg);
		goto out;
	}

	if (ctx->only_2xx && (resp.code < 200 || resp.code >= 300)) {
		pool_release();
		http_send_error(ctx, "HTTP response was not 2xx");
		goto out;
	}

	if (resp.content_type[0] != '\0') {
		if (!http_send_header(ctx, "Content-Type: %s",
				      resp.content_type))
			goto out;
	} else {
		if (!http_send_header(ctx, "Content-Type: "
				      "application/octet-stream"))
			goto out;
	}

	if (resp.content_length >= 0) {
		if (!http_send_header(ctx, "Content-Length: %lld",
				      resp.content_length))
			goto out;
	}

	if (resp.chunked) {
		ok = http_send_body_chunked(ctx, &reader);
	} else if (resp.content_length >= 0) {
		ok = http_send_body_length(ctx, &reader, resp.content_length);
	} else {
		ok = http_send_body_close(ctx, &reader);
		resp.close_after = true;
	}

	if (ok) {
		http_send_finished(ctx);
	} else {
		pool_release();
		NSLOG(netsurf, INFO, "http: body of %s failed",
		      nsurl_access(ctx->url));
	}

	if (resp.close_after)
		pool_release();

out:
	if (host != NULL)
		lwc_string_unref(host);
	if (port != NULL)
		lwc_string_unref(port);
	free(target);
}

/* ------------------------------------------------------------------ */
/* fetcher interface                                                  */
/* ------------------------------------------------------------------ */

static bool fetch_http_initialise(lwc_string *scheme)
{
	NSLOG(netsurf, INFO, "fetch_http_initialise called for %s",
	      lwc_string_data(scheme));
	return true;
}

static void fetch_http_finalise(lwc_string *scheme)
{
	NSLOG(netsurf, INFO, "fetch_http_finalise called for %s",
	      lwc_string_data(scheme));

	for (int i = 0; i < POOL_SLOTS; i++) {
		if (pool[i].conn != NULL) {
			http_conn_free(pool[i].conn);
			pool[i].conn = NULL;
		}
	}
	pool_current = -1;
}

static bool fetch_http_can_fetch(const nsurl *url)
{
	enum nsurl_scheme_type type = nsurl_get_scheme_type(url);

	return type == NSURL_SCHEME_HTTP || type == NSURL_SCHEME_HTTPS;
}

static void *fetch_http_setup(struct fetch *parent_fetch, nsurl *url,
			      bool only_2xx, bool downgrade_tls,
			      const char *post_urlenc,
			      const struct fetch_multipart_data *post_multipart,
			      const char **headers)
{
	struct fetch_http_context *ctx;

	/* POST and custom headers are not supported yet. */
	if (post_urlenc != NULL || post_multipart != NULL)
		return NULL;

	(void)downgrade_tls;
	(void)headers;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return NULL;

	ctx->fetchh = parent_fetch;
	ctx->url = nsurl_ref(url);
	ctx->only_2xx = only_2xx;

	RING_INSERT(ring, ctx);

	return ctx;
}

static bool fetch_http_start(void *ctx)
{
	struct fetch_http_context *c = ctx;

	NSLOG(fetch, DEBUG, "http: GET %s", nsurl_access(c->url));
	return true;
}

static void fetch_http_abort(void *ctx)
{
	struct fetch_http_context *c = ctx;

	c->aborted = true;
}

static void fetch_http_free(void *ctx)
{
	struct fetch_http_context *c = ctx;

	nsurl_unref(c->url);
	free(c);
}

static void fetch_http_poll(lwc_string *scheme)
{
	struct fetch_http_context *c;

	(void)scheme;

	/* One transaction per poll. A transaction blocks for the whole
	 * round trip, so draining the ring here would keep the browser
	 * from drawing until every object of a page had arrived. */
	if (ring == NULL)
		return;

	c = ring;
	RING_REMOVE(ring, c);

	if (c->locked) {
		RING_INSERT(ring, c);
		return;
	}

	if (!c->aborted)
		fetch_http_transaction(c);

	fetch_remove_from_queues(c->fetchh);
	fetch_free(c->fetchh);
}

nserror fetch_http_register(void)
{
	lwc_string *scheme = lwc_string_ref(corestring_lwc_http);
	lwc_string *secure = lwc_string_ref(corestring_lwc_https);
	const struct fetcher_operation_table fetcher_ops = {
		.initialise = fetch_http_initialise,
		.acceptable = fetch_http_can_fetch,
		.setup = fetch_http_setup,
		.start = fetch_http_start,
		.abort = fetch_http_abort,
		.free = fetch_http_free,
		.poll = fetch_http_poll,
		.finalise = fetch_http_finalise
	};
	nserror error;

	error = fetcher_add(scheme, &fetcher_ops);
	if (error != NSERROR_OK)
		return error;

	return fetcher_add(secure, &fetcher_ops);
}
