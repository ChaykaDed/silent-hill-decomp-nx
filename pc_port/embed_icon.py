"""Inject icon into NRO using PIL (pillow) for image handling.

Reads the NRO, embeds the icon PNG data into the asset section
icon slot, and updates the icon size in the asset header.
"""
import struct, sys, os
from PIL import Image

PAGE = 0x1000

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <nro> <icon.png>")
        sys.exit(1)

    nro_path = sys.argv[1]
    icon_path = sys.argv[2]

    # Validate icon
    img = Image.open(icon_path)
    if img.size != (256, 256):
        print(f"WARNING: icon is {img.size[0]}x{img.size[1]}, expected 256x256")
    del img

    with open(icon_path, 'rb') as f:
        icon_data = f.read()

    with open(nro_path, 'rb') as f:
        data = bytearray(f.read())

    asset_off = struct.unpack('<I', data[0x30:0x34])[0]
    asset_sz  = struct.unpack('<I', data[0x34:0x38])[0]

    # Find the icon slot in the asset header
    icon_slot_off = struct.unpack_from('<I', data, asset_off + 0x10)[0]

    # Place icon at the end of the asset section (after last valid data)
    # Find end of valid data
    last = asset_sz - 1
    while last > 0 and data[asset_off + last] == 0:
        last -= 1
    data_end = last + 1  # first byte past valid data

    # Icon goes here, aligned to 4 bytes
    icon_place = (data_end + 3) & ~3

    new_asset_sz = icon_place + len(icon_data)
    new_asset_sz = (new_asset_sz + PAGE - 1) // PAGE * PAGE

    # Expand file if needed
    if asset_off + new_asset_sz > len(data):
        data.extend(b'\x00' * (asset_off + new_asset_sz - len(data)))

    # Write icon data
    data[asset_off + icon_place:asset_off + icon_place + len(icon_data)] = icon_data

    # Update asset header
    struct.pack_into('<I', data, 0x34, new_asset_sz)
    struct.pack_into('<II', data, asset_off + 0x10, icon_place, len(icon_data))

    with open(nro_path, 'wb') as f:
        f.write(data)

    print(f"Icon: slot=0x{icon_slot_off:X} -> placed at 0x{icon_place:X}")
    print(f"Asset sz: 0x{asset_sz:X} -> 0x{new_asset_sz:X}")
    print(f"Icon size: {len(icon_data)} bytes")
    print("Done")

if __name__ == "__main__":
    main()
