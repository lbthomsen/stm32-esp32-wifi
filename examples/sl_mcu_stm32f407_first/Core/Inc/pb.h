/*
 * pb.h - minimal protobuf encoder/decoder (no malloc).
 *
 * Just enough of the protobuf wire format for the esp_hosted RPC messages:
 * varints (int32, uint32, bool, enum) and length delimited fields (bytes,
 * strings, nested messages).
 */

#ifndef PB_H_
#define PB_H_

#include <stdint.h>
#include <stddef.h>

#define PB_WT_VARINT    0
#define PB_WT_64BIT     1
#define PB_WT_LEN       2
#define PB_WT_32BIT     5

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t pos;
    int error;
} pb_writer_t;

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t pos;
    int error;
} pb_reader_t;

typedef struct {
    uint32_t field;
    uint8_t wire_type;
    uint64_t varint;            // PB_WT_VARINT, PB_WT_64BIT, PB_WT_32BIT
    const uint8_t *data;        // PB_WT_LEN
    size_t len;                 // PB_WT_LEN
} pb_field_t;

void pb_writer_init(pb_writer_t *w, uint8_t *buf, size_t size);
void pb_write_varint_field(pb_writer_t *w, uint32_t field, uint64_t value);
void pb_write_int32_field(pb_writer_t *w, uint32_t field, int32_t value);
void pb_write_bytes_field(pb_writer_t *w, uint32_t field, const void *data, size_t len);

// Nested messages: start returns a marker, end patches in the length.
// Nested messages are limited to 127 bytes per level for single byte lengths,
// longer ones are moved into place (handled internally).
size_t pb_write_nested_start(pb_writer_t *w, uint32_t field);
void pb_write_nested_end(pb_writer_t *w, size_t marker);

void pb_reader_init(pb_reader_t *r, const uint8_t *buf, size_t size);
int pb_read_field(pb_reader_t *r, pb_field_t *f);   // 1 = got field, 0 = end, -1 = error

int32_t pb_as_int32(const pb_field_t *f);

#endif /* PB_H_ */
