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
* mp_decode.h -- minimal MessagePack decoder/encoder (msgpack.org spec, no
* external dependencies). Used to talk the Tarantool binary protocol:
*   - decode: responses (MAP/ARRAY/STR/INT/UINT/BOOL/NIL/DBL/BIN)
*   - encode: request bodies (IPROTO map with uint keys and str values)
*
* All integer types are read in network byte order. Binary data returned by
* the decoder is borrowed (points into the source buffer); callers must copy
* it if they need it to outlive the buffer.
*
*/

#ifndef TNT_MP_DECODE_H
#define TNT_MP_DECODE_H

#include <stddef.h>
#include <stdint.h>
#include <switch.h>

typedef enum {
	TNT_MP_NIL = 0,		/* nil */
	TNT_MP_BOOL,		/* boolean */
	TNT_MP_INT,			/* signed integer (i) */
	TNT_MP_UINT,		/* unsigned integer (u) */
	TNT_MP_DBL,			/* float/double (d) */
	TNT_MP_STR,			/* string (ptr/len) */
	TNT_MP_BIN,			/* raw bytes (ptr/len) */
	TNT_MP_EXT,			/* MessagePack extension (ext/ptr/len) */
	TNT_MP_MAP,			/* map header only (len = field count) */
	TNT_MP_ARRAY		/* array header only (len = element count) */
} tnt_mp_type_t;

typedef struct tnt_mp_val {
	tnt_mp_type_t type;
	int64_t i;			/* TNT_MP_INT */
	uint64_t u;			/* TNT_MP_UINT */
	double d;			/* TNT_MP_DBL */
	int b;				/* TNT_MP_BOOL */
	int ext;			/* TNT_MP_EXT subtype (Tarantool: 0x00..0x04) */
	const uint8_t *ptr;	/* TNT_MP_STR / TNT_MP_BIN / TNT_MP_EXT payload (borrowed) */
	size_t len;			/* STR/BIN/EXT payload length; MAP/ARRAY element count */
} tnt_mp_val_t;

/* Sequential reader over a buffer. */
typedef struct tnt_mp_reader {
	const uint8_t *buf;
	size_t size;
	size_t pos;
} tnt_mp_reader_t;

SWITCH_DECLARE(void) tnt_mp_reader_init(tnt_mp_reader_t *r, const void *buf, size_t size);
SWITCH_DECLARE(size_t) tnt_mp_remaining(const tnt_mp_reader_t *r);

/* Decode the next value. For MAP/ARRAY only the header is consumed; the
 * caller walks elements with subsequent tnt_mp_next()/tnt_mp_skip() calls.
 * Returns 0 on success, -1 on malformed input or end of buffer. */
SWITCH_DECLARE(int) tnt_mp_next(tnt_mp_reader_t *r, tnt_mp_val_t *v);

/* Skip one complete value (recursively, including map/array contents). */
SWITCH_DECLARE(int) tnt_mp_skip(tnt_mp_reader_t *r);

/* Skip the CONTENTS of an already-decoded map/array header (no-op for
 * scalar values). Use this after tnt_mp_next() when the value's payload
 * must be drained from the stream. */
SWITCH_DECLARE(int) tnt_mp_skip_contents(tnt_mp_reader_t *r, const tnt_mp_val_t *v);

/* Growable-free fixed-size writer. Returns 0 on success, -1 on overflow. */
typedef struct tnt_mp_writer {
	uint8_t *buf;
	size_t cap;
	size_t len;
} tnt_mp_writer_t;

SWITCH_DECLARE(void) tnt_mp_writer_init(tnt_mp_writer_t *w, void *buf, size_t cap);
SWITCH_DECLARE(size_t) tnt_mp_writer_len(const tnt_mp_writer_t *w);
SWITCH_DECLARE(int) tnt_mp_write_nil(tnt_mp_writer_t *w);
SWITCH_DECLARE(int) tnt_mp_write_bool(tnt_mp_writer_t *w, int b);
SWITCH_DECLARE(int) tnt_mp_write_uint(tnt_mp_writer_t *w, uint64_t v);
SWITCH_DECLARE(int) tnt_mp_write_int(tnt_mp_writer_t *w, int64_t v);
SWITCH_DECLARE(int) tnt_mp_write_str(tnt_mp_writer_t *w, const char *s, size_t n);
SWITCH_DECLARE(int) tnt_mp_write_map_header(tnt_mp_writer_t *w, uint32_t n);
SWITCH_DECLARE(int) tnt_mp_write_array_header(tnt_mp_writer_t *w, uint32_t n);

#endif /* TNT_MP_DECODE_H */
