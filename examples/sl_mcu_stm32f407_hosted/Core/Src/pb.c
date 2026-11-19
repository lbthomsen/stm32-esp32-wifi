/*
 * pb.c - minimal protobuf encoder/decoder (no malloc).
 *
 * Protobuf wire format in brief: a message is a list of fields, each a key
 * (field number << 3 | wire type, as a varint) followed by the value:
 *   wire type 0: varint (7 bits per byte, low bits first, top bit = more)
 *   wire type 2: length (varint) + that many bytes (bytes, string, message)
 *   wire types 1 and 5: fixed 8 and 4 byte little endian values
 * The writer fills a caller supplied buffer and sets `error` when it runs
 * out; the reader walks a buffer field by field without copying.
 */

#include <string.h>
#include "pb.h"

// Append one byte, or flag an error when the buffer is full.
static void put_byte(pb_writer_t *w, uint8_t b) {
    if (w->pos >= w->size) {
        w->error = 1;
        return;
    }
    w->buf[w->pos++] = b;
}

// Append v as a varint.
static void put_varint(pb_writer_t *w, uint64_t v) {
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) {
            b |= 0x80;
        }
        put_byte(w, b);
    } while (v);
}

// Number of bytes v takes as a varint.
static size_t varint_size(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

// Start writing a message into buf.
void pb_writer_init(pb_writer_t *w, uint8_t *buf, size_t size) {
    w->buf = buf;
    w->size = size;
    w->pos = 0;
    w->error = 0;
}

// Field with a varint value (uint32, uint64, bool, enum).
void pb_write_varint_field(pb_writer_t *w, uint32_t field, uint64_t value) {
    put_varint(w, ((uint64_t) field << 3) | PB_WT_VARINT);
    put_varint(w, value);
}

// Field with an int32 value.
void pb_write_int32_field(pb_writer_t *w, uint32_t field, int32_t value) {
    // Negative int32 is sign extended to 64 bits on the wire.
    pb_write_varint_field(w, field, (uint64_t) (int64_t) value);
}

// Length delimited field (bytes or string).
void pb_write_bytes_field(pb_writer_t *w, uint32_t field, const void *data, size_t len) {
    put_varint(w, ((uint64_t) field << 3) | PB_WT_LEN);
    put_varint(w, len);
    if (w->error || w->pos + len > w->size) {
        w->error = 1;
        return;
    }
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
}

// Begin a nested message: write the key and reserve one byte for the
// length, which isn't known yet. Returns where that byte is.
size_t pb_write_nested_start(pb_writer_t *w, uint32_t field) {
    put_varint(w, ((uint64_t) field << 3) | PB_WT_LEN);
    size_t marker = w->pos;
    put_byte(w, 0);     // placeholder for a one byte length
    return marker;
}

// End a nested message: fill in its length. If the length needs more than
// the one reserved byte, move the body up first.
void pb_write_nested_end(pb_writer_t *w, size_t marker) {
    if (w->error) {
        return;
    }
    size_t len = w->pos - marker - 1;
    size_t extra = varint_size(len) - 1;
    if (extra) {
        // Length needs more than one byte: shift the body up to make room.
        if (w->pos + extra > w->size) {
            w->error = 1;
            return;
        }
        memmove(w->buf + marker + 1 + extra, w->buf + marker + 1, len);
        w->pos += extra;
    }
    size_t p = marker;
    size_t v = len;
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) {
            b |= 0x80;
        }
        w->buf[p++] = b;
    } while (v);
}

// Start reading the message in buf.
void pb_reader_init(pb_reader_t *r, const uint8_t *buf, size_t size) {
    r->buf = buf;
    r->size = size;
    r->pos = 0;
    r->error = 0;
}

// Read a varint. Returns 0, or -1 if the buffer ends inside it or it is
// longer than 64 bits.
static int get_varint(pb_reader_t *r, uint64_t *out) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->pos >= r->size) {
            return -1;
        }
        uint8_t b = r->buf[r->pos++];
        v |= (uint64_t) (b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

// Read the next field into f. Returns 1, 0 at the end of the message, -1 on
// malformed input (and stays in error). Length delimited values point into
// the buffer (f->data, f->len); nested messages are read by starting another
// reader on them.
int pb_read_field(pb_reader_t *r, pb_field_t *f) {
    if (r->error) {
        return -1;
    }
    if (r->pos >= r->size) {
        return 0;
    }

    uint64_t key;
    if (get_varint(r, &key)) {
        r->error = 1;
        return -1;
    }
    memset(f, 0, sizeof(*f));
    f->field = (uint32_t) (key >> 3);
    f->wire_type = key & 7;

    switch (f->wire_type) {
    case PB_WT_VARINT:
        if (get_varint(r, &f->varint)) {
            r->error = 1;
            return -1;
        }
        break;
    case PB_WT_LEN: {
        uint64_t len;
        if (get_varint(r, &len) || len > r->size - r->pos) {
            r->error = 1;
            return -1;
        }
        f->data = r->buf + r->pos;
        f->len = (size_t) len;
        r->pos += (size_t) len;
        break;
    }
    case PB_WT_32BIT:
        if (r->size - r->pos < 4) {
            r->error = 1;
            return -1;
        }
        f->varint = r->buf[r->pos] | (r->buf[r->pos + 1] << 8) | (r->buf[r->pos + 2] << 16) | ((uint32_t) r->buf[r->pos + 3] << 24);
        r->pos += 4;
        break;
    case PB_WT_64BIT:
        if (r->size - r->pos < 8) {
            r->error = 1;
            return -1;
        }
        f->varint = 0;
        for (int i = 7; i >= 0; --i) {
            f->varint = (f->varint << 8) | r->buf[r->pos + i];
        }
        r->pos += 8;
        break;
    default:
        r->error = 1;
        return -1;
    }
    return 1;
}

// A varint field as int32 (negative values arrive sign extended to 64 bits).
int32_t pb_as_int32(const pb_field_t *f) {
    return (int32_t) (uint32_t) f->varint;
}
