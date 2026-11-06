# Decode "DUMP name w h" ... "END name" base64 RGB565 blocks from a serial log into PNGs.
import base64, re, struct, sys
from PIL import Image
log = open(sys.argv[1], 'rb').read().decode(errors='replace').splitlines()
out = sys.argv[2] if len(sys.argv) > 2 else '.'
i = 0
while i < len(log):
    m = re.search(r'DUMP (\S+) (\d+) (\d+)', log[i])
    if not m:
        i += 1; continue
    name, w, h = m.group(1), int(m.group(2)), int(m.group(3))
    data = bytearray(); i += 1
    while i < len(log) and not log[i].startswith('END'):
        line = re.sub(r'[^A-Za-z0-9+/=]', '', log[i])
        try: data += base64.b64decode(line)
        except Exception: pass
        i += 1
    px = struct.unpack('<%dH' % (len(data)//2), bytes(data[:w*h*2]))
    img = Image.new('RGB', (w, h))
    img.putdata([(((v>>11)&31)<<3, ((v>>5)&63)<<2, (v&31)<<3) for v in px] + [(0,0,0)]*(w*h-len(px)))
    img.save(f'{out}/{name}.png'); print(name, len(data), 'bytes')
