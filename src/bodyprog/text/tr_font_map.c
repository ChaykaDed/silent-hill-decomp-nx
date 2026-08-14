#include "bodyprog/text/tr_font_map.h"

// Slot → Unicode lookup for all 84 TR font glyphs (slots 0-83)
// Index: slot index [0..83]
// Value: Unicode codepoint (0 = empty/unused)
static const uint32_t s_slot_to_unicode[TR_GLYPH_COUNT] = {
    0x0430,  // [ 0] U+0430
    0x0028,  // [ 1] '('
    0x0029,  // [ 2] ')'
    0x0431,  // [ 3] U+0431
    0x002B,  // [ 4] '+'
    0x002C,  // [ 5] ','
    0x002D,  // [ 6] '-'
    0x002E,  // [ 7] '.'
    0x002F,  // [ 8] '/'
    0x0030,  // [ 9] '0'
    0x0031,  // [10] '1'
    0x0032,  // [11] '2'
    0x0033,  // [12] '3'
    0x0034,  // [13] '4'
    0x0035,  // [14] '5'
    0x0036,  // [15] '6'
    0x0037,  // [16] '7'
    0x0038,  // [17] '8'
    0x0039,  // [18] '9'
    0x003A,  // [19] ':'
    0x0433,  // [20] U+0433
    0x042D,  // [21] U+042D
    0x042E,  // [22] U+042E
    0x042F,  // [23] U+042F
    0x003F,  // [24] '?'
    0x002A,  // [25] '*'
    0x0410,  // [26] U+0410
    0x0411,  // [27] U+0411
    0x0412,  // [28] U+0412
    0x0413,  // [29] U+0413
    0x0414,  // [30] U+0414
    0x0415,  // [31] U+0415
    0x0416,  // [32] U+0416
    0x0417,  // [33] U+0417
    0x0418,  // [34] U+0418
    0x0419,  // [35] U+0419
    0x041A,  // [36] U+041A
    0x041B,  // [37] U+041B
    0x041C,  // [38] U+041C
    0x041D,  // [39] U+041D
    0x041E,  // [40] U+041E
    0x041F,  // [41] U+041F
    0x0420,  // [42] U+0420
    0x0421,  // [43] U+0421
    0x0422,  // [44] U+0422
    0x0423,  // [45] U+0423
    0x0424,  // [46] U+0424
    0x0425,  // [47] U+0425
    0x0426,  // [48] U+0426
    0x0427,  // [49] U+0427
    0x0428,  // [50] U+0428
    0x0429,  // [51] U+0429
    0x042A,  // [52] U+042A
    0x042B,  // [53] U+042B
    0x0022,  // [54] '"'
    0x0432,  // [55] U+0432
    0x0000,  // [56] empty
    0x0000,  // [57] empty
    0x0434,  // [58] U+0434
    0x0436,  // [59] U+0436
    0x0437,  // [60] U+0437
    0x0438,  // [61] U+0438
    0x0439,  // [62] U+0439
    0x0435,  // [63] U+0435
    0x043B,  // [64] U+043B
    0x043C,  // [65] U+043C
    0x043D,  // [66] U+043D
    0x043E,  // [67] U+043E
    0x043F,  // [68] U+043F
    0x0440,  // [69] U+0440
    0x0441,  // [70] U+0441
    0x0442,  // [71] U+0442
    0x0443,  // [72] U+0443
    0x0444,  // [73] U+0444
    0x044D,  // [74] U+044D
    0x0446,  // [75] U+0446
    0x0447,  // [76] U+0447
    0x0448,  // [77] U+0448
    0x0449,  // [78] U+0449
    0x044A,  // [79] U+044A
    0x044B,  // [80] U+044B
    0x044C,  // [81] U+044C
    0x0445,  // [82] U+0445
    0x044E,  // [83] U+044E
};

// Reverse lookup for Cyrillic block (U+0400-U+04FF)
// Index: (codepoint - 0x400)
// Value: TR byte (0 = unmapped)
static const uint8_t s_cyrillic_to_tr[256] = {
    0x00,     // U+0400 unmapped
    0x46,  // U+0401
    0x00,     // U+0402 unmapped
    0x00,     // U+0403 unmapped
    0x00,     // U+0404 unmapped
    0x00,     // U+0405 unmapped
    0x00,     // U+0406 unmapped
    0x00,     // U+0407 unmapped
    0x00,     // U+0408 unmapped
    0x00,     // U+0409 unmapped
    0x00,     // U+040A unmapped
    0x00,     // U+040B unmapped
    0x00,     // U+040C unmapped
    0x00,     // U+040D unmapped
    0x00,     // U+040E unmapped
    0x00,     // U+040F unmapped
    0x41,  // U+0410
    0x42,  // U+0411
    0x43,  // U+0412
    0x44,  // U+0413
    0x45,  // U+0414
    0x46,  // U+0415
    0x47,  // U+0416
    0x48,  // U+0417
    0x49,  // U+0418
    0x4A,  // U+0419
    0x4B,  // U+041A
    0x4C,  // U+041B
    0x4D,  // U+041C
    0x4E,  // U+041D
    0x4F,  // U+041E
    0x50,  // U+041F
    0x51,  // U+0420
    0x52,  // U+0421
    0x53,  // U+0422
    0x54,  // U+0423
    0x55,  // U+0424
    0x56,  // U+0425
    0x57,  // U+0426
    0x58,  // U+0427
    0x59,  // U+0428
    0x5A,  // U+0429
    0x5B,  // U+042A
    0x5C,  // U+042B
    0x00,     // U+042C unmapped
    0x3C,  // U+042D
    0x3D,  // U+042E
    0x3E,  // U+042F
    0x27,  // U+0430
    0x2A,  // U+0431
    0x5E,  // U+0432
    0x3B,  // U+0433
    0x61,  // U+0434
    0x66,  // U+0435
    0x62,  // U+0436
    0x63,  // U+0437
    0x64,  // U+0438
    0x65,  // U+0439
    0x4B,  // U+043A
    0x67,  // U+043B
    0x68,  // U+043C
    0x69,  // U+043D
    0x6A,  // U+043E
    0x6B,  // U+043F
    0x6C,  // U+0440
    0x6D,  // U+0441
    0x6E,  // U+0442
    0x6F,  // U+0443
    0x70,  // U+0444
    0x79,  // U+0445
    0x72,  // U+0446
    0x73,  // U+0447
    0x74,  // U+0448
    0x75,  // U+0449
    0x76,  // U+044A
    0x77,  // U+044B
    0x78,  // U+044C
    0x71,  // U+044D
    0x7A,  // U+044E
    0x3E,  // U+044F
    0x00,     // U+0450 unmapped
    0x66,  // U+0451
    0x00,     // U+0452 unmapped
    0x00,     // U+0453 unmapped
    0x00,     // U+0454 unmapped
    0x00,     // U+0455 unmapped
    0x00,     // U+0456 unmapped
    0x00,     // U+0457 unmapped
    0x00,     // U+0458 unmapped
    0x00,     // U+0459 unmapped
    0x00,     // U+045A unmapped
    0x00,     // U+045B unmapped
    0x00,     // U+045C unmapped
    0x00,     // U+045D unmapped
    0x00,     // U+045E unmapped
    0x00,     // U+045F unmapped
    0x00,     // U+0460 unmapped
    0x00,     // U+0461 unmapped
    0x00,     // U+0462 unmapped
    0x00,     // U+0463 unmapped
    0x00,     // U+0464 unmapped
    0x00,     // U+0465 unmapped
    0x00,     // U+0466 unmapped
    0x00,     // U+0467 unmapped
    0x00,     // U+0468 unmapped
    0x00,     // U+0469 unmapped
    0x00,     // U+046A unmapped
    0x00,     // U+046B unmapped
    0x00,     // U+046C unmapped
    0x00,     // U+046D unmapped
    0x00,     // U+046E unmapped
    0x00,     // U+046F unmapped
    0x00,     // U+0470 unmapped
    0x00,     // U+0471 unmapped
    0x00,     // U+0472 unmapped
    0x00,     // U+0473 unmapped
    0x00,     // U+0474 unmapped
    0x00,     // U+0475 unmapped
    0x00,     // U+0476 unmapped
    0x00,     // U+0477 unmapped
    0x00,     // U+0478 unmapped
    0x00,     // U+0479 unmapped
    0x00,     // U+047A unmapped
    0x00,     // U+047B unmapped
    0x00,     // U+047C unmapped
    0x00,     // U+047D unmapped
    0x00,     // U+047E unmapped
    0x00,     // U+047F unmapped
    0x00,     // U+0480 unmapped
    0x00,     // U+0481 unmapped
    0x00,     // U+0482 unmapped
    0x00,     // U+0483 unmapped
    0x00,     // U+0484 unmapped
    0x00,     // U+0485 unmapped
    0x00,     // U+0486 unmapped
    0x00,     // U+0487 unmapped
    0x00,     // U+0488 unmapped
    0x00,     // U+0489 unmapped
    0x00,     // U+048A unmapped
    0x00,     // U+048B unmapped
    0x00,     // U+048C unmapped
    0x00,     // U+048D unmapped
    0x00,     // U+048E unmapped
    0x00,     // U+048F unmapped
    0x00,     // U+0490 unmapped
    0x00,     // U+0491 unmapped
    0x00,     // U+0492 unmapped
    0x00,     // U+0493 unmapped
    0x00,     // U+0494 unmapped
    0x00,     // U+0495 unmapped
    0x00,     // U+0496 unmapped
    0x00,     // U+0497 unmapped
    0x00,     // U+0498 unmapped
    0x00,     // U+0499 unmapped
    0x00,     // U+049A unmapped
    0x00,     // U+049B unmapped
    0x00,     // U+049C unmapped
    0x00,     // U+049D unmapped
    0x00,     // U+049E unmapped
    0x00,     // U+049F unmapped
    0x00,     // U+04A0 unmapped
    0x00,     // U+04A1 unmapped
    0x00,     // U+04A2 unmapped
    0x00,     // U+04A3 unmapped
    0x00,     // U+04A4 unmapped
    0x00,     // U+04A5 unmapped
    0x00,     // U+04A6 unmapped
    0x00,     // U+04A7 unmapped
    0x00,     // U+04A8 unmapped
    0x00,     // U+04A9 unmapped
    0x00,     // U+04AA unmapped
    0x00,     // U+04AB unmapped
    0x00,     // U+04AC unmapped
    0x00,     // U+04AD unmapped
    0x00,     // U+04AE unmapped
    0x00,     // U+04AF unmapped
    0x00,     // U+04B0 unmapped
    0x00,     // U+04B1 unmapped
    0x00,     // U+04B2 unmapped
    0x00,     // U+04B3 unmapped
    0x00,     // U+04B4 unmapped
    0x00,     // U+04B5 unmapped
    0x00,     // U+04B6 unmapped
    0x00,     // U+04B7 unmapped
    0x00,     // U+04B8 unmapped
    0x00,     // U+04B9 unmapped
    0x00,     // U+04BA unmapped
    0x00,     // U+04BB unmapped
    0x00,     // U+04BC unmapped
    0x00,     // U+04BD unmapped
    0x00,     // U+04BE unmapped
    0x00,     // U+04BF unmapped
    0x00,     // U+04C0 unmapped
    0x00,     // U+04C1 unmapped
    0x00,     // U+04C2 unmapped
    0x00,     // U+04C3 unmapped
    0x00,     // U+04C4 unmapped
    0x00,     // U+04C5 unmapped
    0x00,     // U+04C6 unmapped
    0x00,     // U+04C7 unmapped
    0x00,     // U+04C8 unmapped
    0x00,     // U+04C9 unmapped
    0x00,     // U+04CA unmapped
    0x00,     // U+04CB unmapped
    0x00,     // U+04CC unmapped
    0x00,     // U+04CD unmapped
    0x00,     // U+04CE unmapped
    0x00,     // U+04CF unmapped
    0x00,     // U+04D0 unmapped
    0x00,     // U+04D1 unmapped
    0x00,     // U+04D2 unmapped
    0x00,     // U+04D3 unmapped
    0x00,     // U+04D4 unmapped
    0x00,     // U+04D5 unmapped
    0x00,     // U+04D6 unmapped
    0x00,     // U+04D7 unmapped
    0x00,     // U+04D8 unmapped
    0x00,     // U+04D9 unmapped
    0x00,     // U+04DA unmapped
    0x00,     // U+04DB unmapped
    0x00,     // U+04DC unmapped
    0x00,     // U+04DD unmapped
    0x00,     // U+04DE unmapped
    0x00,     // U+04DF unmapped
    0x00,     // U+04E0 unmapped
    0x00,     // U+04E1 unmapped
    0x00,     // U+04E2 unmapped
    0x00,     // U+04E3 unmapped
    0x00,     // U+04E4 unmapped
    0x00,     // U+04E5 unmapped
    0x00,     // U+04E6 unmapped
    0x00,     // U+04E7 unmapped
    0x00,     // U+04E8 unmapped
    0x00,     // U+04E9 unmapped
    0x00,     // U+04EA unmapped
    0x00,     // U+04EB unmapped
    0x00,     // U+04EC unmapped
    0x00,     // U+04ED unmapped
    0x00,     // U+04EE unmapped
    0x00,     // U+04EF unmapped
    0x00,     // U+04F0 unmapped
    0x00,     // U+04F1 unmapped
    0x00,     // U+04F2 unmapped
    0x00,     // U+04F3 unmapped
    0x00,     // U+04F4 unmapped
    0x00,     // U+04F5 unmapped
    0x00,     // U+04F6 unmapped
    0x00,     // U+04F7 unmapped
    0x00,     // U+04F8 unmapped
    0x00,     // U+04F9 unmapped
    0x00,     // U+04FA unmapped
    0x00,     // U+04FB unmapped
    0x00,     // U+04FC unmapped
    0x00,     // U+04FD unmapped
    0x00,     // U+04FE unmapped
    0x00,     // U+04FF unmapped
};

// Reverse lookup for ASCII printable range (0x20-0x7E)
// Index: codepoint - 0x20
// Value: TR byte (0 = unmapped)
static const uint8_t s_ascii_to_tr[95] = {
    0x5F,  // ' ' U+0020
    0x00,     // U+0021 unmapped
    0x5D,  // '"' U+0022
    0x00,     // U+0023 unmapped
    0x00,     // U+0024 unmapped
    0x00,     // U+0025 unmapped
    0x00,     // U+0026 unmapped
    0x00,     // U+0027 unmapped
    0x28,  // '(' U+0028
    0x29,  // ')' U+0029
    0x40,  // '*' U+002A
    0x2B,  // '+' U+002B
    0x2C,  // ',' U+002C
    0x2D,  // '-' U+002D
    0x2E,  // '.' U+002E
    0x2F,  // '/' U+002F
    0x30,  // '0' U+0030
    0x31,  // '1' U+0031
    0x32,  // '2' U+0032
    0x33,  // '3' U+0033
    0x34,  // '4' U+0034
    0x35,  // '5' U+0035
    0x36,  // '6' U+0036
    0x37,  // '7' U+0037
    0x38,  // '8' U+0038
    0x39,  // '9' U+0039
    0x3A,  // ':' U+003A
    0x00,     // U+003B unmapped
    0x00,     // U+003C unmapped
    0x00,     // U+003D unmapped
    0x00,     // U+003E unmapped
    0x3F,  // '?' U+003F
    0x00,     // U+0040 unmapped
    0x00,     // U+0041 unmapped
    0x00,     // U+0042 unmapped
    0x00,     // U+0043 unmapped
    0x00,     // U+0044 unmapped
    0x00,     // U+0045 unmapped
    0x00,     // U+0046 unmapped
    0x00,     // U+0047 unmapped
    0x00,     // U+0048 unmapped
    0x00,     // U+0049 unmapped
    0x00,     // U+004A unmapped
    0x00,     // U+004B unmapped
    0x00,     // U+004C unmapped
    0x00,     // U+004D unmapped
    0x00,     // U+004E unmapped
    0x00,     // U+004F unmapped
    0x00,     // U+0050 unmapped
    0x00,     // U+0051 unmapped
    0x00,     // U+0052 unmapped
    0x00,     // U+0053 unmapped
    0x00,     // U+0054 unmapped
    0x00,     // U+0055 unmapped
    0x00,     // U+0056 unmapped
    0x00,     // U+0057 unmapped
    0x00,     // U+0058 unmapped
    0x00,     // U+0059 unmapped
    0x00,     // U+005A unmapped
    0x00,     // U+005B unmapped
    0x00,     // U+005C unmapped
    0x00,     // U+005D unmapped
    0x00,     // U+005E unmapped
    0x00,     // U+005F unmapped
    0x00,     // U+0060 unmapped
    0x00,     // U+0061 unmapped
    0x00,     // U+0062 unmapped
    0x00,     // U+0063 unmapped
    0x00,     // U+0064 unmapped
    0x00,     // U+0065 unmapped
    0x00,     // U+0066 unmapped
    0x00,     // U+0067 unmapped
    0x00,     // U+0068 unmapped
    0x00,     // U+0069 unmapped
    0x00,     // U+006A unmapped
    0x00,     // U+006B unmapped
    0x00,     // U+006C unmapped
    0x00,     // U+006D unmapped
    0x00,     // U+006E unmapped
    0x00,     // U+006F unmapped
    0x00,     // U+0070 unmapped
    0x00,     // U+0071 unmapped
    0x00,     // U+0072 unmapped
    0x00,     // U+0073 unmapped
    0x00,     // U+0074 unmapped
    0x00,     // U+0075 unmapped
    0x00,     // U+0076 unmapped
    0x00,     // U+0077 unmapped
    0x00,     // U+0078 unmapped
    0x00,     // U+0079 unmapped
    0x00,     // U+007A unmapped
    0x00,     // U+007B unmapped
    0x00,     // U+007C unmapped
    0x00,     // U+007D unmapped
    0x00,     // U+007E unmapped
};

uint8_t tr_encode_codepoint(uint32_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E) {
        return s_ascii_to_tr[cp - 0x20];
    }
    if (cp >= 0x0400 && cp <= 0x04FF) {
        return s_cyrillic_to_tr[cp - 0x0400];
    }
    return 0;
}

uint32_t tr_decode_byte(uint8_t b)
{
    if (b == 0x5F) return 0x0020;  // '_' = space in TR renderer
    if (b < TR_GLYPH_MIN || b > TR_GLYPH_MAX)
        return 0;
    return s_slot_to_unicode[b - TR_GLYPH_MIN];
}

static uint32_t utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    if (!*p) return 0;
    uint32_t cp;
    if (p[0] < 0x80) {
        cp = p[0];
        *s += 1;
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *s += 2;
    } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *s += 3;
    } else {
        cp = 0;
        *s += 1;
    }
    return cp;
}

size_t tr_encode_string(const char *utf8, uint8_t *out, size_t out_max)
{
    size_t written = 0;
    while (*utf8 && written < out_max) {
        const char *prev = utf8;
        uint32_t cp = utf8_next(&utf8);
        if (cp == 0) continue;
        uint8_t tr = tr_encode_codepoint(cp);
        if (tr == 0) {
            // Codepoint not in TR font; skip
            continue;
        }
        out[written++] = tr;
    }
    return written;
}