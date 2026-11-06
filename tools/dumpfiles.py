# Extract "FILE name" ... "ENDFILE" base64 blocks from a serial log into files.
import base64, re, sys
log = open(sys.argv[1], 'rb').read().decode(errors='replace').splitlines()
out = sys.argv[2] if len(sys.argv) > 2 else '.'
i = 0
while i < len(log):
    m = re.search(r'FILE (\S+)', log[i])
    if not m or 'ENDFILE' in log[i]:
        i += 1; continue
    data = bytearray(); i += 1
    while i < len(log) and 'ENDFILE' not in log[i]:
        data += base64.b64decode(re.sub(r'[^A-Za-z0-9+/=]', '', log[i]))
        i += 1
    open(f'{out}/{m.group(1)}', 'wb').write(data)
    print(m.group(1), len(data), 'bytes')
