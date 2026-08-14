#ifndef TR_FONT_MAP_H
#define TR_FONT_MAP_H

#include <stdint.h>
#include <stddef.h>

// TR font encoding range
#define TR_GLYPH_MIN 0x27
#define TR_GLYPH_MAX 0x7A
#define TR_GLYPH_COUNT 84

// Encode a single Unicode codepoint to TR font byte.
// Returns 0 if codepoint cannot be encoded.
uint8_t tr_encode_codepoint(uint32_t codepoint);

// Decode a TR font byte back to Unicode codepoint.
// Returns 0 if byte is not in valid range.
uint32_t tr_decode_byte(uint8_t tr_byte);

// Encode a null-terminated UTF-8 string to TR font encoding.
// Returns number of bytes written (excluding null).
size_t tr_encode_string(const char *utf8, uint8_t *out, size_t out_max);

#endif // TR_FONT_MAP_H