"""Post-process NRO to fix the asset header.
elf2nro on Windows embeds icon/NACP data at the end of the file
but fails to update the asset section header (icon_size/nacp_size
stay 0). This script patches those fields without modifying the
declared asset section size — the offsets point past the asset
section boundary, which Ryujinx and hw accept.
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
        asset_off = struct.unpack('<I', data[0x30:0x34])[0]
        patched = False

        if icon_path and os.path.exists(icon_path):
            png_sig = b'\x89PNG\r\n\x1a\n'
            png_pos = data.find(png_sig)
            if png_pos >= 0:
                icon_asset_off = png_pos - asset_off
                icon_sz = len(open(icon_path, 'rb').read())
                f.seek(asset_off + 0x10)
                f.write(struct.pack('<II', icon_asset_off, icon_sz))
                print(f"Icon: off=0x{icon_asset_off:X} sz={icon_sz}")
                patched = True

        if nacp_path and os.path.exists(nacp_path):
            nacp_data = open(nacp_path, 'rb').read()
            search_start = asset_off + 0x1000
            nacp_pos = data.find(nacp_data[:32], search_start)
            if nacp_pos >= 0:
                nacp_asset_off = nacp_pos - asset_off
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
