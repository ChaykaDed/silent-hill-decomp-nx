"""Embed icon into NRO by writing into ASET header area.

NRO has two asset systems:
1. Legacy header (first 0x20 bytes of asset section) — offsets relative to asset_off
2. ASET header (at rel offset stored at NRO+0x34) — modern header with
   icon/NACP/romfs offsets relative to ASET itself.

elf2nro places icon/NACP data after the ASET header but doesn't fill
in the ASET header fields. This script corrects that.
"""
import struct, sys, os

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <nro> <icon.png>")
        sys.exit(1)

    nro_path, icon_path = sys.argv[1], sys.argv[2]

    with open(icon_path, 'rb') as f:
        icon_data = f.read()

    with open(nro_path, 'rb') as f:
        data = bytearray(f.read())

    asset_off    = struct.unpack('<I', data[0x30:0x34])[0]
    aset_off_rel = struct.unpack('<I', data[0x34:0x38])[0]
    aset_off     = asset_off + aset_off_rel

    print(f"Asset: 0x{asset_off:X}  ASET: 0x{aset_off:X}")

    if data[aset_off:aset_off+4] != b'ASET':
        print("ERROR: No ASET magic!")
        sys.exit(1)

    # Check if icon already present after ASET header
    png_sig = b'\x89PNG\r\n\x1a\n'
    icon_in_file = data.find(png_sig, aset_off)

    if icon_in_file >= 0 and icon_in_file < aset_off + 0x100000:
        icon_data_off = icon_in_file - aset_off
        print(f"Icon already at ASET+0x{icon_data_off:X}")
    else:
        icon_data_off = 0x38  # standard: right after ASET header
        end_needed = aset_off + icon_data_off + len(icon_data)
        if end_needed > len(data):
            data.extend(b'\x00' * (end_needed - len(data)))
        data[aset_off + icon_data_off:aset_off + icon_data_off + len(icon_data)] = icon_data
        print(f"Icon written at ASET+0x{icon_data_off:X}")

    # Update ASET header (icon offset at +0x08, icon size at +0x0C)
    struct.pack_into('<II', data, aset_off + 0x08, icon_data_off, len(icon_data))

    # Update legacy header (icon offset at asset+0x10, size at asset+0x14)
    legacy_icon_off = aset_off_rel + icon_data_off
    struct.pack_into('<II', data, asset_off + 0x10, legacy_icon_off, len(icon_data))

    with open(nro_path, 'wb') as f:
        f.write(data)

    print(f"Icon: ASET+0x{icon_data_off:X} sz={len(icon_data)}")
    print(f"Legacy: asset+0x{legacy_icon_off:X}")
    print("Done")

if __name__ == "__main__":
    main()
