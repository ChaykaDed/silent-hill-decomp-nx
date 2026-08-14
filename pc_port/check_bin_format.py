import os
tr = 'C:\\Users\\Georg\\AppData\\Roaming\\Ryujinx\\sdcard\\gamedata\\Silent Hill (ViT Co_Metallist) (Team Raccoon).bin'
orig = 'X:\\Silent Hill (USA).bin'
for label, path in [('TR', tr), ('ORIG', orig)]:
    sz = os.path.getsize(path)
    print(f'{label}: size={sz}, /2048={sz/2048}, /2352={sz/2352}')
    with open(path, 'rb') as f:
        h = f.read(32)
    print(f'  first 32: {" ".join(f"{b:02X}" for b in h)}')
    print()
