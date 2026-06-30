import struct, sys
sys.stdout.reconfigure(encoding='utf-8')

import sys; sys.stdout.reconfigure(encoding='utf-8')
with open('X:/SH1-switch/silent-hill-decomp-nx/pc_port/build_switch/SilentHillPC_icon.nro', 'rb') as f:
    data = f.read()

# Search for PNG magic in the NRO
png_sig = b'\x89PNG\r\n\x1a\n'
pos = data.find(png_sig)
if pos >= 0:
    w = struct.unpack('>I', data[pos+16:pos+20])[0]
    h = struct.unpack('>I', data[pos+20:pos+24])[0]
    print(f'PNG at 0x{pos:X}: {w}x{h}')
else:
    print('No PNG in NRO')

# Search for "Silent Hill" string
sh_pos = data.find(b'Silent Hill')
if sh_pos >= 0:
    print(f'"Silent Hill" at 0x{sh_pos:X}')
else:
    print('No "Silent Hill" in NRO')

# Check NACP file content
with open('X:/SH1-switch/silent-hill-decomp-nx/pc_port/build_switch/SilentHillPC.nacp', 'rb') as f:
    nacp = f.read()
print(f'NACP on disk: {len(nacp)} bytes')
# Search for nacp content in NRO
nacp_hdr = nacp[:16]
npos = data.find(nacp_hdr)
if npos >= 0:
    print(f'NACP content at 0x{npos:X}')
    # Check what's nearby
    nearby = data[max(0,npos-32):npos+64]
    print(f'Nearby: {nearby.hex()}')
else:
    print('NACP content not found in NRO')

# Check the asset section thoroughly
asset_off = struct.unpack('<I', data[0x30:0x34])[0]
asset_sz  = struct.unpack('<I', data[0x38:0x3C])[0]
print(f'\nAsset section: off=0x{asset_off:X} sz=0x{asset_sz:X}')
print(f'File size: {len(data)}')

# The asset blob starts at asset_off
# The asset header struct:
#   0x00: unk_offset (u32)
#   0x04: unk_size (u32)
#   0x08: nacp_offset (u32)
#   0x0C: nacp_size (u32)
#   0x10: icon_offset (u32)
#   0x14: icon_size (u32)
#   0x18: romfs_offset (u32)
#   0x1C: romfs_size (u32)

ah = data[asset_off:asset_off+0x20]
unk_off, unk_sz, nacp_off, nacp_sz, icon_off, icon_sz, romfs_off, romfs_sz = struct.unpack_from('<IIIIIIII', ah, 0)
print(f'\nHeader values:')
print(f'  unk:    off=0x{unk_off:X} sz={unk_sz}')
print(f'  nacp:   off=0x{nacp_off:X} sz={nacp_sz}')
print(f'  icon:   off=0x{icon_off:X} sz={icon_sz}')
print(f'  romfs:  off=0x{romfs_off:X} sz={romfs_sz}')

if icon_sz > 0:
    real_icon_off = asset_off + icon_off
    icon_data = data[real_icon_off:real_icon_off+16]
    print(f'Icon data: {icon_data.hex()}')
    if icon_data[:4] == b'\x89PNG':
        iw = struct.unpack('>I', icon_data[16:20])[0]
        ih = struct.unpack('>I', icon_data[20:24])[0]
        print(f'Icon PNG: {iw}x{ih}')
else:
    print('\nIcon size is 0 - checking what is at icon offset:')
    if icon_off > 0:
        real_icon_off = asset_off + icon_off
        chunk = data[real_icon_off:real_icon_off+64]
        print(f'  At 0x{real_icon_off:X}: {chunk[:32].hex()}')
