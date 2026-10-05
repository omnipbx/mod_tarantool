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
* mp_decode.c -- minimal MessagePack decoder/encoder
*
*/

#include "mp_decode.h"

#include <string.h>

/* ---------- helpers ---------- */

static uint16_t rd_u16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd_u64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

static int need(tnt_mp_reader_t *r, size_t n)
{
	if (r->size - r->pos < n)
		return -1;
	return 0;
}

/* ---------- reader ---------- */

SWITCH_DECLARE(void) tnt_mp_reader_init(tnt_mp_reader_t *r, const void *buf, size_t size)
{
	r->buf = (const uint8_t *)buf;
	r->size = size;
	r->pos = 0;
}

SWITCH_DECLARE(size_t) tnt_mp_remaining(const tnt_mp_reader_t *r)
{
	return r->size - r->pos;
}

SWITCH_DECLARE(int) tnt_mp_next(tnt_mp_reader_t *r, tnt_mp_val_t *v)
{
	uint8_t c;

	memset(v, 0, sizeof(*v));
	if (need(r, 1))
		return -1;
	c = r->buf[r->pos++];

	if (c <= 0x7f) {			/* positive fixint */
		v->type = TNT_MP_UINT;
		v->u = c;
	} else if (c >= 0xe0) {		/* negative fixint */
		v->type = TNT_MP_INT;
		v->i = (int8_t)c;
	} else if (c >= 0xa0 && c <= 0xbf) {	/* fixstr */
		v->type = TNT_MP_STR;
		v->len = c & 0x1f;
		if (need(r, v->len))
			return -1;
		v->ptr = r->buf + r->pos;
		r->pos += v->len;
	} else if (c >= 0x90 && c <= 0x9f) {	/* fixarray */
		v->type = TNT_MP_ARRAY;
		v->len = c & 0x0f;
	} else if (c >= 0x80 && c <= 0x8f) {	/* fixmap */
		v->type = TNT_MP_MAP;
		v->len = c & 0x0f;
	} else {
		switch (c) {
		case 0xc0:				/* nil */
			v->type = TNT_MP_NIL;
			break;
		case 0xc2:				/* false */
			v->type = TNT_MP_BOOL;
			v->b = 0;
			break;
		case 0xc3:				/* true */
			v->type = TNT_MP_BOOL;
			v->b = 1;
			break;
		case 0xca: {			/* float32 */
			uint32_t u;

			if (need(r, 4))
				return -1;
			u = rd_u32(r->buf + r->pos);
			r->pos += 4;
			v->type = TNT_MP_DBL;
			{
				union { uint32_t u; float f; } x;
				x.u = u;
				v->d = x.f;
			}
			break;
		}
		case 0xcb: {			/* float64 */
			uint64_t u;

			if (need(r, 8))
				return -1;
			u = rd_u64(r->buf + r->pos);
			r->pos += 8;
			v->type = TNT_MP_DBL;
			{
				union { uint64_t u; double d; } x;
				x.u = u;
				v->d = x.d;
			}
			break;
		}
		case 0xcc:				/* uint8 */
			if (need(r, 1))
				return -1;
			v->type = TNT_MP_UINT;
			v->u = r->buf[r->pos++];
			break;
		case 0xcd:				/* uint16 */
			if (need(r, 2))
				return -1;
			v->type = TNT_MP_UINT;
			v->u = rd_u16(r->buf + r->pos);
			r->pos += 2;
			break;
		case 0xce:				/* uint32 */
			if (need(r, 4))
				return -1;
			v->type = TNT_MP_UINT;
			v->u = rd_u32(r->buf + r->pos);
			r->pos += 4;
			break;
		case 0xcf:				/* uint64 */
			if (need(r, 8))
				return -1;
			v->type = TNT_MP_UINT;
			v->u = rd_u64(r->buf + r->pos);
			r->pos += 8;
			break;
		case 0xd0:				/* int8 */
			if (need(r, 1))
				return -1;
			v->type = TNT_MP_INT;
			v->i = (int8_t)r->buf[r->pos++];
			break;
		case 0xd1:				/* int16 */
			if (need(r, 2))
				return -1;
			v->type = TNT_MP_INT;
			v->i = (int16_t)rd_u16(r->buf + r->pos);
			r->pos += 2;
			break;
		case 0xd2:				/* int32 */
			if (need(r, 4))
				return -1;
			v->type = TNT_MP_INT;
			v->i = (int32_t)rd_u32(r->buf + r->pos);
			r->pos += 4;
			break;
		case 0xd3:				/* int64 */
			if (need(r, 8))
				return -1;
			v->type = TNT_MP_INT;
			v->i = (int64_t)rd_u64(r->buf + r->pos);
			r->pos += 8;
			break;
		case 0xc4:				/* bin8 */
		case 0xd9:				/* str8 */
			if (need(r, 1))
				return -1;
			v->len = r->buf[r->pos++];
			v->type = (c == 0xc4) ? TNT_MP_BIN : TNT_MP_STR;
			goto payload;
		case 0xc5:				/* bin16 */
		case 0xda:				/* str16 */
			if (need(r, 2))
				return -1;
			v->len = rd_u16(r->buf + r->pos);
			r->pos += 2;
			v->type = (c == 0xc5) ? TNT_MP_BIN : TNT_MP_STR;
			goto payload;
		case 0xc6:				/* bin32 */
		case 0xdb:				/* str32 */
			if (need(r, 4))
				return -1;
			v->len = rd_u32(r->buf + r->pos);
			r->pos += 4;
			v->type = (c == 0xc6) ? TNT_MP_BIN : TNT_MP_STR;
			goto payload;
		case 0xdc:				/* array16 */
			if (need(r, 2))
				return -1;
			v->type = TNT_MP_ARRAY;
			v->len = rd_u16(r->buf + r->pos);
			r->pos += 2;
			break;
		case 0xdd:				/* array32 */
			if (need(r, 4))
				return -1;
			v->type = TNT_MP_ARRAY;
			v->len = rd_u32(r->buf + r->pos);
			r->pos += 4;
			break;
		case 0xde:				/* map16 */
			if (need(r, 2))
				return -1;
			v->type = TNT_MP_MAP;
			v->len = rd_u16(r->buf + r->pos);
			r->pos += 2;
			break;
		case 0xdf:				/* map32 */
			if (need(r, 4))
				return -1;
			v->type = TNT_MP_MAP;
			v->len = rd_u32(r->buf + r->pos);
			r->pos += 4;
			break;
		case 0xd4:				/* fixext1 */
		case 0xd5:				/* fixext2 */
		case 0xd6:				/* fixext4 */
		case 0xd7:				/* fixext8 */
		case 0xd8:				/* fixext16 */
			v->type = TNT_MP_EXT;
			v->len = (size_t)1 << (c - 0xd4);
			if (need(r, 1 + v->len))
				return -1;
			v->ext = r->buf[r->pos++];
			v->ptr = r->buf + r->pos;
			r->pos += v->len;
			break;
		case 0xc7:				/* ext8 */
		case 0xc8:				/* ext16 */
		case 0xc9:				/* ext32 */
			v->type = TNT_MP_EXT;
			if (c == 0xc7) {
				if (need(r, 1))
					return -1;
				v->len = r->buf[r->pos++];
			} else if (c == 0xc8) {
				if (need(r, 2))
					return -1;
				v->len = rd_u16(r->buf + r->pos);
				r->pos += 2;
			} else {
				if (need(r, 4))
					return -1;
				v->len = rd_u32(r->buf + r->pos);
				r->pos += 4;
			}
			if (need(r, 1 + v->len))
				return -1;
			v->ext = r->buf[r->pos++];
			v->ptr = r->buf + r->pos;
			r->pos += v->len;
			break;
		default:				/* reserved */
			return -1;
		}
	}

	return 0;

payload:
	if (need(r, v->len))
		return -1;
	v->ptr = r->buf + r->pos;
	r->pos += v->len;
	return 0;
}

SWITCH_DECLARE(int) tnt_mp_skip(tnt_mp_reader_t *r)
{
	tnt_mp_val_t v;
	uint32_t i, n;

	if (tnt_mp_next(r, &v))
		return -1;

	switch (v.type) {
	case TNT_MP_MAP:
		n = (uint32_t)v.len;
		for (i = 0; i < 2 * n; i++) {
			if (tnt_mp_skip(r))
				return -1;
		}
		break;
	case TNT_MP_ARRAY:
		n = (uint32_t)v.len;
		for (i = 0; i < n; i++) {
			if (tnt_mp_skip(r))
				return -1;
		}
		break;
	default:
		/* STR/BIN payload was already consumed by tnt_mp_next() */
		break;
	}
	return 0;
}

SWITCH_DECLARE(int) tnt_mp_skip_contents(tnt_mp_reader_t *r, const tnt_mp_val_t *v)
{
	uint32_t i, n;

	if (v->type == TNT_MP_MAP) {
		n = (uint32_t)v->len;
		for (i = 0; i < 2 * n; i++) {
			if (tnt_mp_skip(r))
				return -1;
		}
	} else if (v->type == TNT_MP_ARRAY) {
		n = (uint32_t)v->len;
		for (i = 0; i < n; i++) {
			if (tnt_mp_skip(r))
				return -1;
		}
	}
	return 0;
}

/* ---------- writer ---------- */

static int wput(tnt_mp_writer_t *w, const void *data, size_t n)
{
	if (w->len + n > w->cap)
		return -1;
	memcpy(w->buf + w->len, data, n);
	w->len += n;
	return 0;
}

static int wbyte(tnt_mp_writer_t *w, uint8_t c)
{
	return wput(w, &c, 1);
}

SWITCH_DECLARE(void) tnt_mp_writer_init(tnt_mp_writer_t *w, void *buf, size_t cap)
{
	w->buf = (uint8_t *)buf;
	w->cap = cap;
	w->len = 0;
}

SWITCH_DECLARE(size_t) tnt_mp_writer_len(const tnt_mp_writer_t *w)
{
	return w->len;
}

SWITCH_DECLARE(int) tnt_mp_write_nil(tnt_mp_writer_t *w)
{
	return wbyte(w, 0xc0);
}

SWITCH_DECLARE(int) tnt_mp_write_bool(tnt_mp_writer_t *w, int b)
{
	return wbyte(w, b ? 0xc3 : 0xc2);
}

SWITCH_DECLARE(int) tnt_mp_write_uint(tnt_mp_writer_t *w, uint64_t v)
{
	uint8_t buf[9];

	if (v <= 0x7f) {
		return wbyte(w, (uint8_t)v);
	} else if (v <= 0xff) {
		buf[0] = 0xcc;
		buf[1] = (uint8_t)v;
		return wput(w, buf, 2);
	} else if (v <= 0xffff) {
		buf[0] = 0xcd;
		buf[1] = (uint8_t)(v >> 8);
		buf[2] = (uint8_t)v;
		return wput(w, buf, 3);
	} else if (v <= 0xffffffffULL) {
		buf[0] = 0xce;
		buf[1] = (uint8_t)(v >> 24);
		buf[2] = (uint8_t)(v >> 16);
		buf[3] = (uint8_t)(v >> 8);
		buf[4] = (uint8_t)v;
		return wput(w, buf, 5);
	} else {
		int i;

		buf[0] = 0xcf;
		for (i = 0; i < 8; i++)
			buf[1 + i] = (uint8_t)(v >> (56 - 8 * i));
		return wput(w, buf, 9);
	}
}

SWITCH_DECLARE(int) tnt_mp_write_int(tnt_mp_writer_t *w, int64_t v)
{
	uint8_t buf[9];

	if (v >= 0)
		return tnt_mp_write_uint(w, (uint64_t)v);
	if (v >= -32)
		return wbyte(w, (uint8_t)(v & 0xff));
	if (v >= INT8_MIN) {
		buf[0] = 0xd0;
		buf[1] = (uint8_t)v;
		return wput(w, buf, 2);
	}
	if (v >= INT16_MIN) {
		buf[0] = 0xd1;
		buf[1] = (uint8_t)(v >> 8);
		buf[2] = (uint8_t)v;
		return wput(w, buf, 3);
	}
	if (v >= INT32_MIN) {
		buf[0] = 0xd2;
		buf[1] = (uint8_t)(v >> 24);
		buf[2] = (uint8_t)(v >> 16);
		buf[3] = (uint8_t)(v >> 8);
		buf[4] = (uint8_t)v;
		return wput(w, buf, 5);
	}
	{
		int i;

		buf[0] = 0xd3;
		for (i = 0; i < 8; i++)
			buf[1 + i] = (uint8_t)((uint64_t)v >> (56 - 8 * i));
		return wput(w, buf, 9);
	}
}

SWITCH_DECLARE(int) tnt_mp_write_str(tnt_mp_writer_t *w, const char *s, size_t n)
{
	uint8_t h[5];
	size_t hl;

	if (n <= 31) {
		h[0] = (uint8_t)(0xa0 | n);
		hl = 1;
	} else if (n <= 0xff) {
		h[0] = 0xd9;
		h[1] = (uint8_t)n;
		hl = 2;
	} else if (n <= 0xffff) {
		h[0] = 0xda;
		h[1] = (uint8_t)(n >> 8);
		h[2] = (uint8_t)n;
		hl = 3;
	} else {
		h[0] = 0xdb;
		h[1] = (uint8_t)(n >> 24);
		h[2] = (uint8_t)(n >> 16);
		h[3] = (uint8_t)(n >> 8);
		h[4] = (uint8_t)n;
		hl = 5;
	}
	if (wput(w, h, hl))
		return -1;
	return wput(w, s, n);
}

SWITCH_DECLARE(int) tnt_mp_write_map_header(tnt_mp_writer_t *w, uint32_t n)
{
	uint8_t h[5];
	size_t hl;

	if (n <= 15) {
		h[0] = (uint8_t)(0x80 | n);
		hl = 1;
	} else if (n <= 0xffff) {
		h[0] = 0xde;
		h[1] = (uint8_t)(n >> 8);
		h[2] = (uint8_t)n;
		hl = 3;
	} else {
		h[0] = 0xdf;
		h[1] = (uint8_t)(n >> 24);
		h[2] = (uint8_t)(n >> 16);
		h[3] = (uint8_t)(n >> 8);
		h[4] = (uint8_t)n;
		hl = 5;
	}
	return wput(w, h, hl);
}

SWITCH_DECLARE(int) tnt_mp_write_array_header(tnt_mp_writer_t *w, uint32_t n)
{
	uint8_t h[5];
	size_t hl;

	if (n <= 15) {
		h[0] = (uint8_t)(0x90 | n);
		hl = 1;
	} else if (n <= 0xffff) {
		h[0] = 0xdc;
		h[1] = (uint8_t)(n >> 8);
		h[2] = (uint8_t)n;
		hl = 3;
	} else {
		h[0] = 0xdd;
		h[1] = (uint8_t)(n >> 24);
		h[2] = (uint8_t)(n >> 16);
		h[3] = (uint8_t)(n >> 8);
		h[4] = (uint8_t)n;
		hl = 5;
	}
	return wput(w, h, hl);
}
