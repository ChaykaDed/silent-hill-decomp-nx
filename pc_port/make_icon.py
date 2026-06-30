"""Generate a 256x256 PNG icon for Silent Hill 1 on Switch.

Creates a minimal icon: dark background with 'SILENT HILL' and the
classic red title treatment — no external dependencies (pure Python stdlib).
"""
import struct, zlib, os

W = H = 256

def make_png(pixels, w, h):
    """Build a RGBA PNG from a flat list of (r,g,b,a) tuples."""
    raw = b""
    for y in range(h):
        raw += b"\x00"  # filter byte
        for x in range(w):
            r, g, b, a = pixels[y * w + x]
            raw += struct.pack("BBBB", r, g, b, a)

    def chunk(ctype, data):
        c = ctype + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    idat = chunk(b"IDAT", zlib.compress(raw))
    iend = chunk(b"IEND", b"")
    return sig + ihdr + idat + iend

def rr(frac):
    return min(255, max(0, int(frac * 255 + 0.5)))

def lerp(a, b, t):
    return a * (1 - t) + b * t

def main():
    bg = (12, 6, 6, 255)     # very dark maroon
    red = (140, 10, 10, 255)  # Silent Hill red
    text_col = (200, 180, 170, 240)  # warm grey-white

    pixels = [bg] * (W * H)

    # Simple raster "S" and "H" using filled rectangles on a 4x8 grid
    # Each char is 3 cells wide, 7 cells tall, one cell gap.
    # Grid: cell = 24x24 px, offset ~ (W - 7*24)//2 horizontally
    # We'll use a pixel grid approach: just draw rectangles.

    # Actually, a cleaner approach: draw horizontal/vertical bars for
    # SILENT HILL text using a coarse bitmap.

    # Simple geometric icon: draw a large stylized "S" and "H" using
    # filled rectangles on a grid, then add a subtitle bar.

    # --- Title bar at the top ---
    bar_h = 28
    bar_y0 = 32
    for y in range(bar_y0, bar_y0 + bar_h):
        for x in range(12, W - 12):
            idx = y * W + x
            t = (x - 12) / (W - 24)
            r = int(lerp(red[0], text_col[0], t))
            g = int(lerp(red[1], text_col[1], t))
            b = int(lerp(red[2], text_col[2], t))
            pixels[idx] = (min(255,r), min(255,g), min(255,b), 255)

    # --- Big "S" (left) and "H" (right) using geometric bars ---
    # S: 3 vertical bars + 3 horizontal bars
    cx, cy = W//2, 140
    def rect(x1, y1, x2, y2, color):
        for y in range(max(0,y1), min(H,y2+1)):
            for x in range(max(0,x1), min(W,x2+1)):
                if y < H and x < W:
                    pixels[y*W+x] = color

    col = (220, 210, 200, 255)

    # "S" — left half
    s_x0 = cx - 72
    s_w = 16
    # top bar
    rect(s_x0, 68, cx - 8, 68 + s_w, col)
    # upper-left vertical
    rect(s_x0, 68, s_x0 + s_w, cy - s_w//2, col)
    # middle bar
    rect(s_x0, cy - s_w//2, cx - 8, cy + s_w//2, col)
    # lower-right vertical
    rect(cx - s_w - 8, cy - s_w//2, cx - 8, 68 + 4*s_w + 16, col)  # too tall, fix

    # Actually this is getting complex. Let me simplify: draw a bold
    # stylized SH using thick rectangles.

    # Clear and draw simple bold letters:
    pixels = [bg] * (W * H)

    # Title bar gradient
    bar_h = 24
    bar_y0 = 36
    for y in range(bar_y0, bar_y0 + bar_h):
        for x in range(16, W - 16):
            idx = y * W + x
            t = (x - 16) / (W - 32)
            r = int(lerp(red[0], text_col[0], t))
            g = int(lerp(red[1], text_col[1], t))
            b = int(lerp(red[2], text_col[2], t))
            pixels[idx] = (min(255,r), min(255,g), min(255,b), 255)

    lw = 18  # letter stroke width
    gap = 12
    ch_w = 48
    let_sp = 20
    s_x = (W - (2 * ch_w + let_sp)) // 2
    h_x = s_x + ch_w + let_sp
    let_top = 80
    let_bot = 200
    let_mid = (let_top + let_bot) // 2

    # S: top, middle, bottom horizontals + left-top vertical + right-bottom vertical
    # top horizontal
    rect(s_x, let_top, s_x + ch_w, let_top + lw, col)
    # bottom horizontal
    rect(s_x, let_bot - lw, s_x + ch_w, let_bot, col)
    # middle horizontal
    rect(s_x, let_mid - lw//2, s_x + ch_w, let_mid + lw//2, col)
    # left vertical (top half)
    rect(s_x, let_top, s_x + lw, let_mid, col)
    # right vertical (bottom half)
    rect(s_x + ch_w - lw, let_mid, s_x + ch_w, let_bot, col)

    # H: two verticals + middle horizontal
    rect(h_x, let_top, h_x + lw, let_bot, col)
    rect(h_x + ch_w - lw, let_top, h_x + ch_w, let_bot, col)
    rect(h_x, let_mid - lw//2, h_x + ch_w, let_mid + lw//2, col)

    # Small "1" in bottom-right of the H block? No, keep it SH.
    # Add a small "1" subscript after the H:
    one_x = h_x + ch_w + 8
    one_w = 14
    rect(one_x, let_bot - 36, one_x + one_w, let_bot, col)
    # small flag on top of 1
    rect(one_x - 4, let_bot - 48, one_x + one_w, let_bot - 36, col)

    # Subtitle: "Silent Hill" in small text area at bottom
    sub_y = 218
    sub_h = 10
    for y in range(sub_y, sub_y + sub_h):
        for x in range(40, W - 40):
            idx = y * W + x
            t = (x - 40) / (W - 80)
            r = int(lerp(red[0], 80, t))
            g = int(lerp(red[1], 60, t))
            b = int(lerp(red[2], 50, t))
            pixels[idx] = (r, g, b, 220)

    png = make_png(pixels, W, H)
    out = os.path.join(os.path.dirname(__file__), "switch_icon.png")
    with open(out, "wb") as f:
        f.write(png)
    print(f"Icon written: {out} ({len(png)} bytes)")

if __name__ == "__main__":
    main()
