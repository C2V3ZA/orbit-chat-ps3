"""Overview of four actual renderer outputs, with fixture data; no external assets."""
from pathlib import Path
from png import chunk
import struct
import zlib

sources = ['ui-chat', 'ui-invite', 'ui-empty', 'ui-menu']
w, h = 1280, 720
sheet = bytearray(w * h * 3)
for index, name in enumerate(sources):
    with (Path('build') / (name + '.ppm')).open('rb') as f:
        assert f.readline() == b'P6\n'
        iw, ih = map(int, f.readline().split())
        assert (iw, ih) == (1280, 720)
        assert f.readline() == b'255\n'
        source = f.read()
    ox, oy = (index % 2) * 640, (index // 2) * 360
    for y in range(360):
        for x in range(640):
            a, b = ((y * 2) * iw + x * 2) * 3, ((oy + y) * w + ox + x) * 3
            sheet[b:b + 3] = source[a:a + 3]
rows = b''.join(b'\0' + sheet[y*w*3:(y+1)*w*3] for y in range(h))
Path('build/ui-overview.png').write_bytes(
    b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w,h,8,2,0,0,0))
    + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
