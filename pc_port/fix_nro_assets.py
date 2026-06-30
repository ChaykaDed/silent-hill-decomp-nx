"""Post-process NRO to fix the asset header.
elf2nro on Windows embeds the icon/NACP data at the end of the file
but fails to update the asset section header (icon_size/nacp_size
stay 0). This script patches those fields.
"""
import struct, sys, os

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <nro> [icon.png] [nacp]")
        sys.exit(1)

    nro = sys.argv[1]
    icon_path = sys.argv[2] if len(sys.argv) > 2 else None
    nacp_path = sys.argv[3] if len(sys.argv) > 3 else None

    with open(nro, 'r+b') as f:
        data = f.read()

        # NRO header fields
        asset_off = struct.unpack('<I', data[0x30:0x34])[0]
        asset_sz  = struct.unpack('<I', data[0x34:0x38])[0]
        file_end  = len(data)

        # Find icon and NACP data in the file (after the asset section)
        png_sig = b'\x89PNG\r\n\x1a\n'
        nacp_sig = None  # NACP has no fixed magic

        patched = False

        if icon_path and os.path.exists(icon_path):
            with open(icon_path, 'rb') as ic:
                icon_data = ic.read()
            png_pos = data.find(png_sig)
            if png_pos >= 0 and png_pos >= asset_off:
                icon_asset_off = png_pos - asset_off
                icon_sz = len(icon_data)
                # Update asset header
                f.seek(asset_off + 0x10)
                f.write(struct.pack('<II', icon_asset_off, icon_sz))
                print(f"Icon: off=0x{icon_asset_off:X} sz={icon_sz}")
                patched = True

                # Expand asset section to cover icon
                new_sz = icon_asset_off + icon_sz
                new_sz = ((new_sz + 0xFFF) // 0x1000) * 0x1000
                if new_sz > asset_sz:
                    f.seek(0x34)
                    f.write(struct.pack('<I', new_sz))
                    print(f"Asset section: 0x{asset_sz:X} -> 0x{new_sz:X}")

        if nacp_path and os.path.exists(nacp_path):
            with open(nacp_path, 'rb') as nc:
                nacp_data = nc.read()
            # Find NACP after the asset section by looking for it
            # NACP doesn't have a magic, so search from
            # asset_off + asset_sz to end for known NACP bytes
            search_start = asset_off + asset_sz
            search_region = data[search_start:]
            # Look for the NACP by matching header bytes
            nacp_pos = search_region.find(nacp_data[:16])
            if nacp_pos >= 0:
                nacp_asset_off = nacp_pos + (search_start - asset_off)
                nacp_sz = len(nacp_data)
                f.seek(asset_off + 0x08)
                f.write(struct.pack('<II', nacp_asset_off, nacp_sz))
                print(f"NACP: off=0x{nacp_asset_off:X} sz={nacp_sz}")
                patched = True

        if patched:
            print("Asset header patched.")
        else:
            print("No patches applied.")

if __name__ == "__main__":
    main()
