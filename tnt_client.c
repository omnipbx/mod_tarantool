/*
* mod_tarantool for FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2026, Filippov Anatoliy <error.email@mail.ru>
*
* Version: MPL 2.0
*
* The contents of this file are subject to the Mozilla Public License Version
* 1.1 (the "License"); you may not use this file except in compliance with
* the License. You may obtain a copy of the License at
* http://www.mozilla.org/MPL/
*
* Software distributed under the License is distributed on an "AS IS" basis,
* WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
* for the specific language governing rights and limitations under the
* License.
*
* The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
*
* The Initial Developer of the Original Code is
* Anthony Minessale II <anthm@freeswitch.org>
* Portions created by the Initial Developer are Copyright (C)
* the Initial Developer. All Rights Reserved.
*
* Contributor(s):
* Filippov Anatoliy <error.email@mail.ru>
*
* tnt_client.c -- minimal IPROTO binary-protocol client
*
*/

#include "tnt_client.h"
#include "mp_decode.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---------- IPROTO constants ---------- */

#define IPROTO_REQUEST_TYPE	0x00
#define IPROTO_SYNC		0x01
#define IPROTO_SPACE_ID		0x10
#define IPROTO_INDEX_ID		0x11
#define IPROTO_KEY		0x20
#define IPROTO_TUPLE		0x21
#define IPROTO_FUNCTION_NAME	0x22
#define IPROTO_USER_NAME	0x23
#define IPROTO_DATA		0x30
#define IPROTO_ERROR_24		0x31
#define IPROTO_METADATA		0x32	/* >= 3.x */
#define IPROTO_SQL_TEXT		0x40
#define IPROTO_SQL_BIND		0x41
#define IPROTO_SQL_INFO		0x42
#define IPROTO_METADATA_OLD	0x52	/* < 3.x; 3.x uses 0x52 for ERROR map */
#define IPROTO_ERROR_STACK	0x71

#define IPROTO_OK		0x00
#define IPROTO_AUTH		0x07
#define IPROTO_EXECUTE		0x0b
#define IPROTO_PING		0x40

#define IPROTO_SQL_INFO_ROW_COUNT	0x00
#define IPROTO_SQL_INFO_AUTOINCREMENT	0x01

#define IPROTO_FIELD_NAME	0x00
#define IPROTO_FIELD_TYPE	0x01

#define TNT_GREETING_SIZE	128
#define TNT_MAX_RESPONSE	(64u * 1024 * 1024)

/* forward declarations (defined later in this file) */
static int parse_response(tnt_conn_t *c, tnt_result_t *res);
static int build_request(tnt_conn_t *c, uint8_t type, tnt_mp_writer_t *w);
static int conn_send_request(tnt_conn_t *c, uint8_t type, const uint8_t *body, size_t body_len);
static int conn_recv_frame(tnt_conn_t *c, uint8_t **body, size_t *body_len);

/* ---------- time helpers ---------- */

static int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *dup_str(const char *s, size_t n)
{
	char *p = (char *)malloc(n + 1);

	if (!p)
		return NULL;
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

/* ---------- SHA-1 (RFC 3174) ---------- */

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
	uint32_t w[80];
	uint32_t a, b, c, d, e, f, k, tmp;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
			((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
	}
	for (i = 16; i < 80; i++) {
		tmp = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
		w[i] = (tmp << 1) | (tmp >> 31);
	}

	a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

	for (i = 0; i < 80; i++) {
		if (i < 20) {
			f = (b & c) | ((~b) & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
		e = d;
		d = c;
		c = (b << 30) | (b >> 2);
		b = a;
		a = tmp;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
	uint32_t state[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
	uint64_t bitlen = (uint64_t)len * 8;
	size_t i = 0;
	uint8_t pad[128];

	memset(pad, 0, sizeof(pad));

	while (len - i >= 64) {
		sha1_transform(state, data + i);
		i += 64;
	}

	/* final block(s): remainder + 0x80 + zeros + 64-bit bit length */
	{
		size_t rem = len - i;
		size_t total = ((rem + 9 + 63) / 64) * 64;
		size_t j;

		memset(pad, 0, sizeof(pad));
		memcpy(pad, data + i, rem);
		pad[rem] = 0x80;
		for (j = 0; j < 8; j++)
			pad[total - 1 - j] = (uint8_t)(bitlen >> (8 * j));

		for (j = 0; j < total; j += 64)
			sha1_transform(state, pad + j);
	}

	for (i = 0; i < 5; i++) {
		out[i * 4] = (uint8_t)(state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)state[i];
	}
}

/* ---------- SHA-256 (FIPS 180-4) ---------- */

static uint32_t rotr32(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
	static const uint32_t K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
		0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
		0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
		0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
		0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
		0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
	};
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h, t1, t2;
	int i;

	for (i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
			((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
	}
	for (i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];

	for (i = 0; i < 64; i++) {
		uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);

		t1 = h + S1 + ch + K[i] + w[i];
		t2 = S0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	uint32_t state[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	uint64_t bitlen = (uint64_t)len * 8;
	size_t i = 0;
	uint8_t pad[128];

	memset(pad, 0, sizeof(pad));

	while (len - i >= 64) {
		sha256_transform(state, data + i);
		i += 64;
	}
	{
		size_t rem = len - i;
		size_t total = ((rem + 9 + 63) / 64) * 64;
		size_t j;

		memset(pad, 0, sizeof(pad));
		memcpy(pad, data + i, rem);
		pad[rem] = 0x80;
		for (j = 0; j < 8; j++)
			pad[total - 1 - j] = (uint8_t)(bitlen >> (8 * j));

		for (j = 0; j < total; j += 64)
			sha256_transform(state, pad + j);
	}

	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)state[i];
	}
}

/* ---------- base64 ---------- */

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

/* Decode base64 without padding ('='); returns number of bytes written. */
static size_t base64_decode(const char *in, size_t inlen, uint8_t *out, size_t outcap)
{
	size_t i = 0, o = 0;
	uint32_t acc = 0;
	int nbits = 0;

	for (i = 0; i < inlen && o < outcap; i++) {
		int v = b64_val(in[i]);

		if (v < 0)
			continue;
		acc = (acc << 6) | (uint32_t)v;
		nbits += 6;
		if (nbits >= 8) {
			nbits -= 8;
			out[o++] = (uint8_t)(acc >> nbits);
		}
	}
	return o;
}

/* ---------- connection ---------- */

static void set_sock_timeouts(int fd, uint32_t ms)
{
	struct timeval tv;

	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

SWITCH_DECLARE(void) tnt_conn_init(tnt_conn_t *c, const tnt_host_cfg_t *h,
								   const char *user, const char *pass,
								   uint32_t rw_timeout_ms)
{
	memset(c, 0, sizeof(*c));
	c->fd = -1;
	c->use_unix = h->use_unix;
	snprintf(c->addr, sizeof(c->addr), "%s", h->addr);
	c->port = h->port;
	snprintf(c->user, sizeof(c->user), "%s", user ? user : "");
	snprintf(c->pass, sizeof(c->pass), "%s", pass ? pass : "");
	c->rw_timeout_ms = rw_timeout_ms;
	c->sync = 1;
}

SWITCH_DECLARE(int) tnt_conn_is_open(const tnt_conn_t *c)
{
	return c->fd >= 0;
}

SWITCH_DECLARE(const char *) tnt_conn_last_error(const tnt_conn_t *c)
{
	return c->error[0] ? c->error : "unknown error";
}

static void conn_seterr(tnt_conn_t *c, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(c->error, sizeof(c->error), fmt, ap);
	va_end(ap);
}

SWITCH_DECLARE(void) tnt_conn_close(tnt_conn_t *c)
{
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

/* Non-blocking connect with timeout via poll(). */
static int sock_connect_timeout(int fd, const struct sockaddr *sa, socklen_t slen,
								uint32_t timeout_ms)
{
	int flags = fcntl(fd, F_GETFL, 0);
	struct pollfd pfd;
	int rc;

	if (flags < 0)
		return -1;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	rc = connect(fd, sa, slen);
	if (rc < 0 && errno != EINPROGRESS)
		return -1;

	pfd.fd = fd;
	pfd.events = POLLOUT;
	do {
		rc = poll(&pfd, 1, (int)timeout_ms);
	} while (rc < 0 && errno == EINTR);
	if (rc <= 0) {
		errno = (rc == 0) ? ETIMEDOUT : errno;
		return -1;
	}
	{
		int soerr = 0;
		socklen_t l = sizeof(soerr);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l) < 0 || soerr != 0) {
			errno = soerr ? soerr : ECONNREFUSED;
			return -1;
		}
	}
	/* restore blocking mode (SO_RCVTIMEO/SO_SNDTIMEO govern timeouts) */
	if (fcntl(fd, F_SETFL, flags) < 0)
		return -1;
	return 0;
}

static int conn_tcp_connect(tnt_conn_t *c, uint32_t timeout_ms)
{
	struct addrinfo hints, *ai = NULL, *p;
	char port[16];
	int fd = -1, rc = -1;

	snprintf(port, sizeof(port), "%u", (unsigned)c->port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(c->addr, port, &hints, &ai) != 0) {
		conn_seterr(c, "getaddrinfo(%s) failed", c->addr);
		return -1;
	}

	for (p = ai; p; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0)
			continue;
		if (sock_connect_timeout(fd, p->ai_addr, p->ai_addrlen, timeout_ms) == 0) {
			rc = 0;
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);

	if (rc == 0) {
		int one = 1;
		set_sock_timeouts(fd, c->rw_timeout_ms);
		setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		c->fd = fd;
	} else {
		conn_seterr(c, "connect(%s:%u) failed: %s", c->addr, c->port, strerror(errno));
	}
	return rc;
}

static int conn_unix_connect(tnt_conn_t *c, uint32_t timeout_ms)
{
	struct sockaddr_un sa;
	int fd;
	int one = 1;

	if (strlen(c->addr) >= sizeof(sa.sun_path)) {
		conn_seterr(c, "unix socket path too long: %s", c->addr);
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		conn_seterr(c, "socket(AF_UNIX) failed: %s", strerror(errno));
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", c->addr);

	if (sock_connect_timeout(fd, (struct sockaddr *)&sa, sizeof(sa), timeout_ms) < 0) {
		conn_seterr(c, "connect(unix:%s) failed: %s", c->addr, strerror(errno));
		close(fd);
		return -1;
	}

	set_sock_timeouts(fd, c->rw_timeout_ms);
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	c->fd = fd;
	return 0;
}

/* Read the 128-byte greeting and verify/parse it.
 *
 * Layout by generation:
 *   2.10 and older: "Tarantool <ver> (<console>)\n" <44-char salt> "\n" <20 bytes>
 *   2.11+/3.x:      "Tarantool <ver> (<proto>) <uuid>  \n" <44-char salt> <pad> "\n"
 * In both layouts the 44 base64 salt chars start immediately after the
 * FIRST '\n', so we extract them from there (padding/tail differs). */
static int conn_read_greeting(tnt_conn_t *c)
{
	char buf[TNT_GREETING_SIZE];
	char salt_b64[48];
	size_t got = 0;
	size_t i;
	uint8_t salt[64];

	while (got < sizeof(buf)) {
		ssize_t n = recv(c->fd, buf + got, sizeof(buf) - got, 0);

		if (n > 0) {
			got += (size_t)n;
			continue;
		}
		if (n == 0) {
			conn_seterr(c, "server closed during greeting");
			return -1;
		}
		if (errno == EINTR)
			continue;
		conn_seterr(c, "greeting read failed: %s", strerror(errno));
		return -1;
	}
	if (memcmp(buf, "Tarantool ", 10) != 0) {
		conn_seterr(c, "not a Tarantool server (greeting mismatch)");
		return -1;
	}

	/* find the first '\n' (end of the version line) */
	for (i = 0; i < sizeof(buf); i++) {
		if (buf[i] == '\n')
			break;
	}
	if (i >= sizeof(buf) || i + 28 >= sizeof(buf)) {
		conn_seterr(c, "malformed greeting (bad salt layout)");
		return -1;
	}

	/* The 20-byte chap-sha1 salt is encoded as the first 27 base64 chars
	 * right after the version line plus a '=' pad (28 chars total); the
	 * remaining chars in the line are extra server data. Verified against
	 * both 2.10 and 3.x greeting layouts. */
	memcpy(salt_b64, buf + i + 1, 27);
	salt_b64[27] = '=';
	salt_b64[28] = '\0';

	memset(salt, 0, sizeof(salt));
	{
		size_t dec = base64_decode(salt_b64, 28, salt, sizeof(salt));

		c->salt_len = 20;
		if (dec != 20) {
			conn_seterr(c, "bad base64 salt in greeting");
			return -1;
		}
		memcpy(c->salt, salt, c->salt_len);
	}
	return 0;
}

/* chap auth: scramble = H(salt || H(H(pass))) XOR H(pass)
 * with H = SHA1 and 20-byte salt (chap-sha1), or
 * with H = SHA256 and 32-byte salt (chap-sha256, default since 3.x). */
static int conn_auth(tnt_conn_t *c)
{
	uint8_t body[512];
	tnt_mp_writer_t w;
	uint8_t *rbody;
	size_t rlen;
	tnt_result_t res;
	int i, rc;
	uint8_t step1[32], step2[32], step3[32], scramble[32];
	uint8_t scratch[64];
	size_t hlen = c->salt_len;

	if (!c->user[0])
		return 0;			/* no user configured: skip explicit auth */

	if (hlen == 20) {
		sha1((const uint8_t *)c->pass, strlen(c->pass), step1);
		sha1(step1, 20, step2);
		memcpy(scratch, c->salt, 20);
		memcpy(scratch + 20, step2, 20);
		sha1(scratch, 40, step3);
	} else {
		/* chap-sha256 (32-byte salt) */
		sha256((const uint8_t *)c->pass, strlen(c->pass), step1);
		sha256(step1, 32, step2);
		memcpy(scratch, c->salt, 32);
		memcpy(scratch + 32, step2, 32);
		sha256(scratch, 64, step3);
	}
	for (i = 0; i < (int)hlen; i++)
		scramble[i] = (uint8_t)(step3[i] ^ step1[i]);

	tnt_mp_writer_init(&w, body, sizeof(body));
	if (build_request(c, IPROTO_AUTH, &w) < 0)
		return -1;
	/* auth body: {IPROTO_USER_NAME: user, IPROTO_TUPLE: [method, scramble]}
	 * Tarantool >= 3.x requires the method name as the first tuple
	 * element; older generations accept a bare [scramble], so sending
	 * the two-element form is safe for 2.11+ and required since 3.0. */
	if (tnt_mp_write_map_header(&w, 2) < 0)
		return -1;
	if (tnt_mp_write_uint(&w, IPROTO_USER_NAME) < 0)
		return -1;
	if (tnt_mp_write_str(&w, c->user, strlen(c->user)) < 0)
		return -1;
	if (tnt_mp_write_uint(&w, IPROTO_TUPLE) < 0)
		return -1;
	if (tnt_mp_write_array_header(&w, 2) < 0)
		return -1;
	if (tnt_mp_write_str(&w, hlen == 20 ? "chap-sha1" : "chap-sha256",
						 hlen == 20 ? 9 : 11) < 0)
		return -1;
	if (tnt_mp_write_str(&w, (const char *)scramble, hlen) < 0)
		return -1;

	if (conn_send_request(c, IPROTO_AUTH, body, w.len) < 0)
		return -1;
	if (conn_recv_frame(c, &rbody, &rlen) < 0)
		return -1;

	memset(&res, 0, sizeof(res));
	rc = parse_response(c, &res);
	if (rc != 0) {
#ifdef TNT_DEBUG_AUTH
		fprintf(stderr, "AUTH rc=%d rlen=%zu body:", rc, c->rsp_len);
		{
			size_t di;
			for (di = 0; di < c->rsp_len; di++)
				fprintf(stderr, "%02x", c->rsp[di]);
			fprintf(stderr, "\n");
		}
#endif
		conn_seterr(c, "auth failed: %s", res.error[0] ? res.error : "permission denied");
		return -1;
	}
	return 0;
}

SWITCH_DECLARE(int) tnt_conn_connect(tnt_conn_t *c, uint32_t connect_timeout_ms)
{
	int rc;

	if (c->fd >= 0)
		return 0;
	rc = c->use_unix ? conn_unix_connect(c, connect_timeout_ms)
					 : conn_tcp_connect(c, connect_timeout_ms);
	if (rc < 0)
		return -1;
	if (conn_read_greeting(c) < 0) {
		tnt_conn_close(c);
		return -1;
	}
	if (conn_auth(c) < 0) {
		tnt_conn_close(c);
		return -1;
	}
	c->last_used_mono = mono_ms();
	return 0;
}

/* ---------- frame I/O ---------- */

static int send_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static int recv_exact(int fd, uint8_t *buf, size_t len)
{
	size_t got = 0;

	while (got < len) {
		ssize_t n = recv(fd, buf + got, len - got, 0);

		if (n > 0) {
			got += (size_t)n;
			continue;
		}
		if (n == 0)
			return -1;		/* peer closed */
		if (errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static int conn_recv_frame(tnt_conn_t *c, uint8_t **body, size_t *body_len)
{
	uint8_t hdr[5];
	uint32_t len;

	if (recv_exact(c->fd, hdr, sizeof(hdr)) < 0) {
		conn_seterr(c, "frame header read failed: %s", strerror(errno));
		return -1;
	}
	if (hdr[0] != 0xce) {
		conn_seterr(c, "unexpected frame header byte 0x%02x", hdr[0]);
		return -1;
	}
	len = ((uint32_t)hdr[1] << 24) | ((uint32_t)hdr[2] << 16) |
		((uint32_t)hdr[3] << 8) | (uint32_t)hdr[4];
	if (len > TNT_MAX_RESPONSE) {
		conn_seterr(c, "response too large (%u bytes)", len);
		return -1;
	}
	if (c->rsp_cap < len) {
		uint8_t *nb = (uint8_t *)realloc(c->rsp, len);

		if (!nb) {
			conn_seterr(c, "out of memory reading response");
			return -1;
		}
		c->rsp = nb;
		c->rsp_cap = len;
	}
	if (recv_exact(c->fd, c->rsp, len) < 0) {
		conn_seterr(c, "frame body read failed: %s", strerror(errno));
		return -1;
	}
	c->rsp_len = len;
	*body = c->rsp;
	*body_len = len;
	return 0;
}

static int conn_send_request(tnt_conn_t *c, uint8_t type, const uint8_t *body, size_t body_len)
{
	uint8_t hdr[5] = { 0xce };
	uint64_t total = 5 + body_len;
	uint8_t *pkt;
	int rc;

	(void)type;

	hdr[1] = (uint8_t)(body_len >> 24);
	hdr[2] = (uint8_t)(body_len >> 16);
	hdr[3] = (uint8_t)(body_len >> 8);
	hdr[4] = (uint8_t)body_len;

	pkt = (uint8_t *)malloc(total);
	if (!pkt) {
		conn_seterr(c, "out of memory building request");
		return -1;
	}
	memcpy(pkt, hdr, 5);
	if (body_len)
		memcpy(pkt + 5, body, body_len);

	rc = send_all(c->fd, pkt, total);
	free(pkt);
	if (rc < 0) {
		conn_seterr(c, "request send failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* Write the request header as a standalone msgpack map: {type, sync}.
 * The caller then appends the request body as a SECOND top-level msgpack
 * object (Tarantool expects header and body as separate objects). */
static int build_request(tnt_conn_t *c, uint8_t type, tnt_mp_writer_t *w)
{
	if (tnt_mp_write_map_header(w, 2) < 0)
		return -1;
	if (tnt_mp_write_uint(w, IPROTO_REQUEST_TYPE) < 0)
		return -1;
	if (tnt_mp_write_uint(w, type) < 0)
		return -1;
	if (tnt_mp_write_uint(w, IPROTO_SYNC) < 0)
		return -1;
	if (tnt_mp_write_uint(w, c->sync) < 0)
		return -1;
	return 0;
}

/* ---------- value formatting (for result rows) ---------- */

/* ---------- MessagePack extension decoders (Tarantool wire format) ----------
 *   ext 0x00 decimal  -- payload layout pending pin-down via the hexdump
 *                        probe (decimal_pack); rendered as raw hex for now.
 *   ext 0x01 error    -- never appears in row data (defensive only).
 *   ext 0x02 datetime -- 14 bytes: scale(1) tzoffset(2, int16 BE, minutes)
 *                        epoch(8, int64 BE, seconds) nsec(3, BE).
 *   ext 0x03 uuid     -- 16 raw bytes, RFC 4122 big-endian.
 *   ext 0x04 interval -- layout pending pin-down via the hexdump probe;
 *                        rendered as raw hex for now.
 * Unknown subtypes fall back to a deterministic hex string. Decoders never
 * fail the parse: malformed payloads produce an empty string. */

static char *fmt_ext_hex(const uint8_t *p, size_t len)
{
	static const char hexd[] = "0123456789abcdef";
	char *out;
	size_t i;

	out = (char *)malloc(len * 2 + 1);
	if (!out)
		return NULL;
	for (i = 0; i < len; i++) {
		out[i * 2] = hexd[p[i] >> 4];
		out[i * 2 + 1] = hexd[p[i] & 0x0f];
	}
	out[len * 2] = '\0';
	return out;
}

/* 16 raw UUID bytes -> 8-4-4-4-12 lowercase (tt console canonical form). */
static char *fmt_ext_uuid(const uint8_t *p, size_t len)
{
	char out[37];

	if (len != 16)
		return NULL;
	snprintf(out, sizeof(out),
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
		p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	return dup_str(out, 36);
}

/* Howard Hinnant's civil_from_days: days since 1970-01-01 -> Y/M/D. */
static void civil_from_days(int64_t z, int *yy, int *mm, int *dd)
{
	int64_t era, y;
	unsigned doe, yoe, doy, mp, d, m;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = (int64_t)yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	if (m <= 2)
		y++;
	*yy = (int)y;
	*mm = (int)m;
	*dd = (int)d;
}

/* Tarantool datetime ext -> ISO 8601 like the tt console renders it:
 * YYYY-MM-DDTHH:MM:SS[.fff...]±HHMM (timezone offset without a colon).
 * Fraction digits and the offset suffix are assembled by hand so the
 * formatter cannot trip -Wformat-truncation. */
static char *fmt_ext_datetime(const uint8_t *p, size_t len)
{
	char out[48], frac[12], tzsuf[8];
	char dig[10];
	int64_t epoch = 0, secs, days, rem;
	int32_t tz;
	uint32_t nsec, fv;
	int scale, i, y, mo, d, h, mi, s, tzh, tzm, ah, k, pad, n;

	if (len != 14)
		return NULL;
	scale = p[0];
	tz = (int16_t)((uint16_t)((p[1] << 8) | p[2]));
	for (i = 3; i < 11; i++)
		epoch = (epoch << 8) | p[i];
	/* NOTE: the 3-byte nsec field carries the fractional part already
	 * scaled to the declared precision (".789" @ scale 3 -> 789), so no
	 * 10^n scaling is applied here. Pending the hexdump probe: if the
	 * server turns out to store raw nanoseconds, restore the division by
	 * 10^(9-scale) before trimming. */
	nsec = ((uint32_t)p[11] << 16) | ((uint32_t)p[12] << 8) | (uint32_t)p[13];

	secs = epoch + (int64_t)tz * 60;
	days = secs / 86400;
	rem = secs % 86400;
	if (rem < 0) {
		rem += 86400;
		days--;
	}
	h = (int)(rem / 3600);
	mi = (int)((rem % 3600) / 60);
	s = (int)(rem % 60);
	civil_from_days(days, &y, &mo, &d);

	/* fraction: trailing zeros are stripped first (tt console style),
	 * then the remaining digits are right-aligned to the scale width */
	frac[0] = '\0';
	fv = nsec;
	if (fv != 0 && scale > 0) {
		if (scale > 9)
			scale = 9;
		while (fv > 0 && fv % 10 == 0) {
			fv /= 10;
			scale--;
		}
		if (fv > 0) {
			k = 0;
			while (fv > 0) {
				dig[k++] = (char)('0' + fv % 10);
				fv /= 10;
			}
			pad = scale - k;
			if (pad < 0)
				pad = 0;
			frac[0] = '.';
			n = 1;
			for (i = 0; i < pad; i++)
				frac[n++] = '0';
			for (i = k - 1; i >= 0; i--)
				frac[n++] = dig[i];
			frac[n] = '\0';
		}
	}

	/* timezone suffix ±HHMM (offset in minutes, split by magnitude) */
	tzsuf[0] = '\0';
	if (tz != 0) {
		ah = tz < 0 ? -tz : tz;
		tzh = ah / 60;
		tzm = ah % 60;
		if (tzh > 99)
			tzh = 99;			/* defensive: real offsets are <= 14h */
		tzsuf[0] = tz < 0 ? '-' : '+';
		tzsuf[1] = (char)('0' + tzh / 10);
		tzsuf[2] = (char)('0' + tzh % 10);
		tzsuf[3] = (char)('0' + tzm / 10);
		tzsuf[4] = (char)('0' + tzm % 10);
		tzsuf[5] = '\0';
	}

	snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d:%02d%s%s",
		y, mo, d, h, mi, s, frac, tzsuf);
	return dup_str(out, strlen(out));
}

static char *fmt_mp_val(const tnt_mp_val_t *v)
{
	char num[64];
	char *s;

	switch (v->type) {
	case TNT_MP_NIL:
		return dup_str("", 0);
	case TNT_MP_BOOL:
		return dup_str(v->b ? "true" : "false", v->b ? 4 : 5);
	case TNT_MP_INT:
		snprintf(num, sizeof(num), "%" PRId64, v->i);
		return dup_str(num, strlen(num));
	case TNT_MP_UINT:
		snprintf(num, sizeof(num), "%" PRIu64, v->u);
		return dup_str(num, strlen(num));
	case TNT_MP_DBL:
		snprintf(num, sizeof(num), "%g", v->d);
		return dup_str(num, strlen(num));
	case TNT_MP_STR:
	case TNT_MP_BIN:
		return dup_str((const char *)v->ptr, v->len);
	case TNT_MP_EXT:
		switch (v->ext) {
		case 0x03:			/* uuid */
			s = fmt_ext_uuid(v->ptr, v->len);
			if (s)
				return s;
			break;
		case 0x02:			/* datetime */
			s = fmt_ext_datetime(v->ptr, v->len);
			if (s)
				return s;
			break;
		case 0x00:			/* decimal */
		case 0x04:			/* interval */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_tarantool: ext 0x%02x (%s) rendered as raw hex, "
				"wire layout not pinned yet\n",
				v->ext, v->ext == 0x00 ? "decimal" : "interval");
			return fmt_ext_hex(v->ptr, v->len);
		default:			/* unknown / error ext */
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				"mod_tarantool: unknown ext type 0x%02x len=%zu, raw hex\n",
				v->ext, v->len);
			return fmt_ext_hex(v->ptr, v->len);
		}
		return dup_str("", 0);
	default:
		return dup_str("", 0);
	}
}

/* ---------- response parsing ---------- */

typedef struct col_meta {
	char *name;
	char *type;
} col_meta_t;

typedef struct raw_row {
	tnt_mp_val_t *vals;
	size_t n;
} raw_row_t;

SWITCH_DECLARE(int) tnt_result_init(tnt_result_t *res, int ncols, int nrows)
{
	int r, c;

	memset(res, 0, sizeof(*res));
	res->ncols = ncols;
	res->nrows = nrows;
	res->affected_rows = -1;

	if (ncols < 0 || nrows < 0)
		return -1;
	res->col_name = (char **)calloc((size_t)ncols, sizeof(char *));
	res->col_type = (char **)calloc((size_t)ncols, sizeof(char *));
	res->rows = (char ***)calloc((size_t)(nrows > 0 ? nrows : 1), sizeof(char **));
	if (!res->col_name || !res->col_type || !res->rows)
		return -1;
	for (r = 0; r < nrows; r++) {
		res->rows[r] = (char **)calloc((size_t)ncols > 0 ? (size_t)ncols : 1, sizeof(char *));
		if (!res->rows[r])
			return -1;
		for (c = 0; c < ncols; c++) {
			res->rows[r][c] = dup_str("", 0);
			if (!res->rows[r][c])
				return -1;
		}
	}
	return 0;
}

SWITCH_DECLARE(void) tnt_result_free(tnt_result_t *res)
{
	int r, c;

	for (c = 0; c < res->ncols; c++) {
		free(res->col_name[c]);
		free(res->col_type[c]);
	}
	free(res->col_name);
	free(res->col_type);
	for (r = 0; r < res->nrows; r++) {
		if (res->rows && res->rows[r]) {
			for (c = 0; c < res->ncols; c++)
				free(res->rows[r][c]);
			free(res->rows[r]);
		}
	}
	free(res->rows);
	memset(res, 0, sizeof(*res));
}

/* Fill in the result from collected metadata + raw data. Returns 0 or -1. */
static int fill_result(tnt_result_t *res, const col_meta_t *cols, int ncols,
					   const raw_row_t *raw, int nraw, int affected)
{
	int r, c;

	if (tnt_result_init(res, ncols, nraw) < 0)
		return -1;
	res->affected_rows = affected;

	for (c = 0; c < ncols; c++) {
		/* deep copy: cols[] are transient and freed by the caller */
		res->col_name[c] = dup_str(cols[c].name ? cols[c].name : "", cols[c].name ? strlen(cols[c].name) : 0);
		res->col_type[c] = dup_str(cols[c].type ? cols[c].type : "", cols[c].type ? strlen(cols[c].type) : 0);
	}
	for (r = 0; r < nraw; r++) {
		for (c = 0; c < ncols; c++) {
			if ((int)raw[r].n > c) {
				char *s = fmt_mp_val(&raw[r].vals[c]);

				free(res->rows[r][c]);
				res->rows[r][c] = s ? s : dup_str("", 0);
			}
		}
	}
	return 0;
}

/* Parse an IPROTO response body. */
static int parse_response(tnt_conn_t *c, tnt_result_t *res)
{
	tnt_mp_reader_t rd;
	tnt_mp_val_t v, k;
	uint64_t rtype = IPROTO_OK;
	uint64_t rsync = 0;
	int have_sync = 0;
	col_meta_t cols[256];
	int ncols = 0;
	raw_row_t *raw = NULL;
	size_t nraw = 0, capraw = 0;
	int affected = -1;
	int ret = 0;

	memset(cols, 0, sizeof(cols));

#ifdef TNT_DEBUG_SQL
	fprintf(stderr, "RSP[%zu]:", c->rsp_len);
	{
		size_t di;
		for (di = 0; di < c->rsp_len && di < 256; di++)
			fprintf(stderr, "%02x", c->rsp[di]);
		fprintf(stderr, "\n");
	}
#endif

	tnt_mp_reader_init(&rd, c->rsp, c->rsp_len);

	/* ---- header map: {type, sync, schema_version, ...} ---- */
	if (tnt_mp_next(&rd, &v) < 0 || v.type != TNT_MP_MAP) {
		conn_seterr(c, "response is not a map");
		ret = -1;
		goto out;
	}
	{
		uint32_t i, n = (uint32_t)v.len;

		for (i = 0; i < n; i++) {
			if (tnt_mp_next(&rd, &k) < 0 || tnt_mp_next(&rd, &v) < 0) {
				ret = -1;
				goto out;
			}
			switch (k.type == TNT_MP_UINT ? (int)k.u : -1) {
			case IPROTO_REQUEST_TYPE:
				if (v.type == TNT_MP_UINT || v.type == TNT_MP_INT)
					rtype = v.u;
				break;
			case IPROTO_SYNC:
				if (v.type == TNT_MP_UINT || v.type == TNT_MP_INT) {
					rsync = v.u;
					have_sync = 1;
				}
				break;
			default:
				if (tnt_mp_skip_contents(&rd, &v) < 0) {
					ret = -1;
					goto out;
				}
				break;
			}
		}
	}

	/* ---- body map: {data, metadata, sql_info, error, ...} ----
	 * Tarantool >= 3.x returns header and body as two separate msgpack
	 * maps; older generations use a single map. Both are accepted here. */
	if (tnt_mp_remaining(&rd) > 0) {
		if (tnt_mp_next(&rd, &v) < 0 || v.type != TNT_MP_MAP) {
			conn_seterr(c, "response body is not a map");
			ret = -1;
			goto out;
		}
		{
			uint32_t i, n = (uint32_t)v.len;

			for (i = 0; i < n; i++) {
				if (tnt_mp_next(&rd, &k) < 0 || tnt_mp_next(&rd, &v) < 0) {
					ret = -1;
					goto out;
				}
				switch (k.type == TNT_MP_UINT ? (int)k.u : -1) {
				case IPROTO_DATA:
					if (v.type == TNT_MP_ARRAY) {
						uint32_t r, nr = (uint32_t)v.len;

						for (r = 0; r < nr; r++) {
							tnt_mp_val_t rowv;
							raw_row_t *rr;

							if (tnt_mp_next(&rd, &rowv) < 0) {
								ret = -1;
								goto out;
							}
							if (nraw >= capraw) {
								size_t ncap = capraw ? capraw * 2 : 16;
								raw_row_t *nb = (raw_row_t *)realloc(raw, ncap * sizeof(*nb));

								if (!nb) {
									ret = -1;
									goto out;
								}
								raw = nb;
								capraw = ncap;
							}
							rr = &raw[nraw];
							memset(rr, 0, sizeof(*rr));
							if (rowv.type == TNT_MP_ARRAY) {
								uint32_t c2, nc = (uint32_t)rowv.len;

								rr->vals = (tnt_mp_val_t *)calloc(nc ? nc : 1, sizeof(tnt_mp_val_t));
								if (!rr->vals) {
									ret = -1;
									goto out;
								}
								rr->n = nc;
								for (c2 = 0; c2 < nc; c2++) {
									if (tnt_mp_next(&rd, &rr->vals[c2]) < 0) {
										ret = -1;
										goto out;
									}
								}
							}
							nraw++;
						}
					}
					break;
				case IPROTO_METADATA:
				case IPROTO_METADATA_OLD:
					/* old 0x52 key also carried the error string on failure */
					if (v.type == TNT_MP_STR && !res->error[0]) {
						size_t n = v.len < sizeof(res->error) - 1 ? v.len : sizeof(res->error) - 1;

						memcpy(res->error, v.ptr, n);
						res->error[n] = '\0';
						break;
					}
					if (v.type != TNT_MP_ARRAY)
						break;
					{
						uint32_t m, nm = (uint32_t)v.len;

						if (nm > 256)
							nm = 256;
						for (m = 0; m < nm; m++) {
							tnt_mp_val_t fv;
							tnt_mp_val_t fk;
							uint32_t j, nj;

							if (tnt_mp_next(&rd, &fv) < 0 || fv.type != TNT_MP_MAP) {
								ret = -1;
								goto out;
							}
							nj = (uint32_t)fv.len;
							for (j = 0; j < nj; j++) {
								if (tnt_mp_next(&rd, &fk) < 0 || tnt_mp_next(&rd, &fv) < 0) {
									ret = -1;
									goto out;
								}
								if (fk.type != TNT_MP_UINT)
									continue;
								if (fk.u == IPROTO_FIELD_NAME && fv.type == TNT_MP_STR) {
									cols[m].name = dup_str((const char *)fv.ptr, fv.len);
								} else if (fk.u == IPROTO_FIELD_TYPE && fv.type == TNT_MP_STR) {
									cols[m].type = dup_str((const char *)fv.ptr, fv.len);
								}
							}
						}
						ncols = (int)nm;
					}
					break;
				case IPROTO_SQL_INFO:
					if (v.type == TNT_MP_MAP) {
						uint32_t j, nj = (uint32_t)v.len;

						for (j = 0; j < nj; j++) {
							tnt_mp_val_t ik, iv;

							if (tnt_mp_next(&rd, &ik) < 0 || tnt_mp_next(&rd, &iv) < 0) {
								ret = -1;
								goto out;
							}
							if (ik.type == TNT_MP_UINT && ik.u == IPROTO_SQL_INFO_ROW_COUNT &&
								(iv.type == TNT_MP_UINT || iv.type == TNT_MP_INT)) {
								affected = (int)iv.u;
							}
						}
					}
					break;
				case IPROTO_ERROR_24:
					if (v.type == TNT_MP_STR && !res->error[0]) {
						size_t n = v.len < sizeof(res->error) - 1 ? v.len : sizeof(res->error) - 1;

						memcpy(res->error, v.ptr, n);
						res->error[n] = '\0';
					}
					break;
				case IPROTO_ERROR_STACK:
					/* array of maps {0x00: type, 0x03: message, ...} */
					if (v.type == TNT_MP_ARRAY) {
						uint32_t e, ne = (uint32_t)v.len;

						for (e = 0; e < ne && !res->error[0]; e++) {
							tnt_mp_val_t ev, ek;
							uint32_t j, nj;

							if (tnt_mp_next(&rd, &ev) < 0) {
								ret = -1;
								goto out;
							}
							if (ev.type != TNT_MP_MAP)
								continue;
							nj = (uint32_t)ev.len;
							for (j = 0; j < nj; j++) {
								if (tnt_mp_next(&rd, &ek) < 0 || tnt_mp_next(&rd, &ev) < 0) {
									ret = -1;
									goto out;
								}
								if (ek.type == TNT_MP_UINT && ek.u == 0x03 &&
									ev.type == TNT_MP_STR && !res->error[0]) {
									size_t n = ev.len < sizeof(res->error) - 1 ?
										ev.len : sizeof(res->error) - 1;

									memcpy(res->error, ev.ptr, n);
									res->error[n] = '\0';
								}
							}
						}
					}
					break;
				default:
					if (tnt_mp_skip_contents(&rd, &v) < 0) {
						ret = -1;
						goto out;
					}
					break;
				}
			}
		}
	}

	if (have_sync && rsync != c->sync) {
		conn_seterr(c, "sync mismatch (%" PRIu64 " != %" PRIu64 ")", rsync, c->sync);
		ret = -1;
		goto out;
	}
	c->sync++;

	if (rtype != IPROTO_OK) {
		if (!res->error[0])
			snprintf(res->error, sizeof(res->error), "tarantool error 0x%04" PRIx64, rtype);
		ret = 1;	/* SQL/protocol error */
		goto out;
	}

	if (fill_result(res, cols, ncols, raw, (int)nraw, affected) < 0) {
		conn_seterr(c, "out of memory filling result");
		ret = -1;
		goto out;
	}

out:
	{
		int i;

		for (i = 0; i < ncols; i++) {
			free(cols[i].name);
			free(cols[i].type);
		}
		if (raw) {
			for (i = 0; i < (int)nraw; i++)
				free(raw[i].vals);
			free(raw);
		}
	}
	return ret;
}

/* ---------- execute / ping ---------- */

SWITCH_DECLARE(int) tnt_conn_execute(tnt_conn_t *c, const char *sql, tnt_result_t *res)
{
	uint8_t body[4096];
	tnt_mp_writer_t w;
	uint8_t *rbody;
	size_t rlen;
	int rc;

	tnt_mp_writer_init(&w, body, sizeof(body));
	if (build_request(c, IPROTO_EXECUTE, &w) < 0)
		return -1;
	/* execute body: {IPROTO_SQL_TEXT: sql} */
	if (tnt_mp_write_map_header(&w, 1) < 0)
		return -1;
	if (tnt_mp_write_uint(&w, IPROTO_SQL_TEXT) < 0)
		return -1;
	if (tnt_mp_write_str(&w, sql, strlen(sql)) < 0)
		return -1;

	if (conn_send_request(c, IPROTO_EXECUTE, body, w.len) < 0)
		return -1;
	if (conn_recv_frame(c, &rbody, &rlen) < 0)
		return -1;
	c->last_used_mono = mono_ms();
	rc = parse_response(c, res);
	return rc;
}

SWITCH_DECLARE(int) tnt_conn_ping(tnt_conn_t *c)
{
	uint8_t body[64];
	tnt_mp_writer_t w;
	uint8_t *rbody;
	size_t rlen;
	tnt_result_t res;
	int rc;

	tnt_mp_writer_init(&w, body, sizeof(body));
	if (build_request(c, IPROTO_PING, &w) < 0)
		return -1;
	if (conn_send_request(c, IPROTO_PING, body, w.len) < 0)
		return -1;
	if (conn_recv_frame(c, &rbody, &rlen) < 0)
		return -1;
	c->last_used_mono = mono_ms();
	memset(&res, 0, sizeof(res));
	rc = parse_response(c, &res);
	return rc < 0 ? -1 : 0;
}

SWITCH_DECLARE(int) tnt_conn_precheck(tnt_conn_t *c)
{
	char ch;

	if (c->fd < 0)
		return -1;
	for (;;) {
		ssize_t n = recv(c->fd, &ch, 1, MSG_PEEK | MSG_DONTWAIT);

		if (n > 0)
			return 1;			/* unexpected data => suspect */
		if (n == 0)
			return 1;			/* peer closed */
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;			/* idle and healthy */
		return -1;
	}
}

/* ---------- session (failover + backoff) ---------- */

/* Fire a "mod_tarantool" custom event: SWITCH_EVENT_CUSTOM with subclass
 * "mod_tarantool", "action" header and optional "profile"/"detail". */
SWITCH_DECLARE(void) tnt_event_emit(const char *action, const char *profile, const char *detail)
{
	switch_event_t *event = NULL;

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, "mod_tarantool") != SWITCH_STATUS_SUCCESS)
		return;
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "action", action);
	if (profile && profile[0])
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "profile", profile);
	if (detail && detail[0])
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "detail", detail);
	switch_event_fire(&event);
}

/* Mark the session broken exactly once and raise the event. */
static void tnt_session_mark_broken(tnt_session_t *s)
{
	if (!s->broken) {
		s->broken = 1;
		tnt_event_emit("session-broken", s->cfg->name, "connection lost");
	}
}

SWITCH_DECLARE(void) tnt_session_init(tnt_session_t *s, const tnt_profile_cfg_t *cfg)
{
	int i;

	memset(s, 0, sizeof(*s));
	s->cfg = cfg;
	s->cur = -1;
	s->conn.fd = -1;
	snprintf(s->user, sizeof(s->user), "%s", cfg->username);
	snprintf(s->pass, sizeof(s->pass), "%s", cfg->password);
	for (i = 0; i < TNT_HOST_MAX; i++) {
		s->dead_until_host[i] = 0;
		s->backoff_host[i] = 0;
	}
}

SWITCH_DECLARE(void) tnt_session_close(tnt_session_t *s)
{
	tnt_conn_close(&s->conn);
}

/* Case-insensitive substring search (ASCII only, no ctype dependency). */
static const char *ci_str(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);
	size_t hl = strlen(hay);

	if (nl > hl)
		return NULL;
	for (size_t i = 0; i + nl <= hl; i++) {
		size_t j;

		for (j = 0; j < nl; j++) {
			char a = hay[i + j];
			char b = needle[j];

			if (a >= 'A' && a <= 'Z')
				a = (char)(a + 32);
			if (b >= 'A' && b <= 'Z')
				b = (char)(b + 32);
			if (a != b)
				break;
		}
		if (j == nl)
			return hay + i;
	}
	return NULL;
}

/* 1 when a SET SESSION error means "this generation does not know the
 * variable" (benign — e.g. sql_seq_scan on 3.x), 0 otherwise. */
static int init_err_benign(const char *err)
{
	if (!err || !err[0])
		return 0;
	return ci_str(err, "does not exist") || ci_str(err, "doesn't exist") ||
		   ci_str(err, "unknown variable") || ci_str(err, "not found") ||
		   ci_str(err, "no such");
}

/* Run the profile-level initialisation on a freshly connected session:
 *   1. the user-supplied init-sql (if any);
 *   2. full-scan enablement: SET SESSION sql_full_scan=true (Tarantool
 *      2.11/3.x) with a sql_seq_scan=true fallback for older 2.x.
 * Unknown-variable errors are treated as benign (the knob simply does not
 * exist on that generation); other errors are logged and ignored so the
 * connection stays usable. Returns 0 on success, -1 on transport failure. */
static int session_run_init(tnt_session_t *s)
{
	static const char *const scan_vars[] = { "sql_full_scan", "sql_seq_scan", NULL };
	tnt_result_t res;
	int vi;

	if (s->cfg->init_sql[0]) {
		memset(&res, 0, sizeof(res));
		if (tnt_conn_execute(&s->conn, s->cfg->init_sql, &res) == 1 &&
			!init_err_benign(res.error)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "mod_tarantool: profile %s: init-sql failed: %s\n",
							  s->cfg->name, res.error[0] ? res.error : "execute error");
		}
		tnt_result_free(&res);
	}

	if (!s->cfg->full_scan)
		return 0;

	for (vi = 0; scan_vars[vi]; vi++) {
		char set[96];
		int rc;

		snprintf(set, sizeof(set), "SET SESSION %s=true", scan_vars[vi]);
		memset(&res, 0, sizeof(res));
		rc = tnt_conn_execute(&s->conn, set, &res);
		if (rc == 1) {
			if (!init_err_benign(res.error)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "mod_tarantool: profile %s: %s failed: %s\n",
								  s->cfg->name, set, res.error[0] ? res.error : "SQL error");
			}
			/* unknown variable -> try the next-generation knob, else done */
			if (!init_err_benign(res.error)) {
				tnt_result_free(&res);
				break;
			}
		} else if (rc == 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
							  "mod_tarantool: profile %s: %s ok\n", s->cfg->name, set);
			tnt_result_free(&res);
			break;					/* the first knob that works wins */
		} else {
			/* transport failure: the connection died during init */
			tnt_result_free(&res);
			return -1;
		}
		tnt_result_free(&res);
	}
	return 0;
}

/* Try to open a connection to host idx; on failure mark it dead. */
static int try_host(tnt_session_t *s, int idx, uint32_t ct_ms)
{
	const tnt_host_cfg_t *h = &s->cfg->hosts[idx];
	int rc;

	tnt_conn_init(&s->conn, h, s->user, s->pass, s->cfg->query_timeout_ms);
	rc = tnt_conn_connect(&s->conn, ct_ms);
	if (rc == 0) {
		s->cur = idx;
		s->backoff_host[idx] = 0;
		s->all_dead = 0;
		if (session_run_init(s) < 0) {
			tnt_conn_close(&s->conn);
			return -1;
		}
		return 0;
	}
	/* mark dead with exponential backoff */
	{
		int64_t now = mono_ms();
		uint32_t b = s->backoff_host[idx];

		if (!b)
			b = 1000;			/* start at 1 s */
		else
			b *= 2;
		if (s->cfg->backoff_max_s && b > s->cfg->backoff_max_s * 1000)
			b = s->cfg->backoff_max_s * 1000;
		s->backoff_host[idx] = b;
		s->dead_until_host[idx] = now + b;
	}
	tnt_conn_close(&s->conn);
	return -1;
}

SWITCH_DECLARE(int) tnt_session_open(tnt_session_t *s)
{
	int64_t now;
	int attempt;
	int start;
	uint32_t ct;

	if (!s->cfg || s->cfg->nhosts <= 0)
		return -1;
	ct = s->cfg->connect_timeout_ms ? s->cfg->connect_timeout_ms : 5000;

	if (s->conn.fd >= 0)
		return 0;				/* already connected */

	now = mono_ms();

	/* start scanning right after the last used host (round-robin) */
	start = (s->cur + 1) % s->cfg->nhosts;
	for (attempt = 0; attempt < s->cfg->nhosts; attempt++) {
		int idx = (start + attempt) % s->cfg->nhosts;

		if (s->dead_until_host[idx] > now)
			continue;			/* still in backoff */
		if (try_host(s, idx, ct) == 0) {
			if (s->broken) {
				s->broken = 0;
				tnt_event_emit("session-recovered", s->cfg->name, "reconnected");
			}
			return 0;
		}
	}

	s->all_dead = 1;
	return -1;
}

SWITCH_DECLARE(int) tnt_session_execute(tnt_session_t *s, const char *sql,
										int retryable, tnt_result_t *res)
{
	int rc;

	if (s->conn.fd < 0 && tnt_session_open(s) < 0)
		return -1;

	/* idle broken-connection detection + PING keepalive */
	{
		int64_t idle = mono_ms() - s->conn.last_used_mono;

		if (idle >= (int64_t)s->cfg->ping_idle_s * 1000 || idle < 0) {
			if (tnt_conn_ping(&s->conn) < 0) {
				tnt_session_mark_broken(s);
				tnt_conn_close(&s->conn);
				if (!retryable)
					return -1;
				if (tnt_session_open(s) < 0)
					return -1;
			}
		} else if (idle > 500) {
			/* quick pre-read check on anything that was idle briefly */
			int pc = tnt_conn_precheck(&s->conn);

			if (pc != 0) {
				tnt_session_mark_broken(s);
				tnt_conn_close(&s->conn);
				if (!retryable)
					return -1;
				if (tnt_session_open(s) < 0)
					return -1;
			}
		}
	}

	rc = tnt_conn_execute(&s->conn, sql, res);
	if (rc == 0 || rc == 1)
		return rc;				/* success or deterministic SQL error */

	/* transport failure */
	tnt_session_mark_broken(s);
	tnt_conn_close(&s->conn);
	if (!retryable)
		return -1;

	if (tnt_session_open(s) < 0)
		return -1;
	/* retry once on the (possibly different) host */
	return tnt_conn_execute(&s->conn, sql, res);
}

SWITCH_DECLARE(int) tnt_session_ping(tnt_session_t *s)
{
	if (s->conn.fd < 0 && tnt_session_open(s) < 0)
		return -1;
	if (tnt_conn_ping(&s->conn) < 0) {
		tnt_conn_close(&s->conn);
		return -1;
	}
	return 0;
}
