"""Check our generated PS3 artifact headers and staged contents; not a console test."""
from pathlib import Path
import struct
import hashlib
import sys
probe = "--probe" in sys.argv
title = "ORB000002" if probe else "ORB000001"
stage = Path("build/probe-pkg" if probe else "build/pkg")
p=Path('build/Orbit-Audio-Test-0.2.pkg' if probe else 'build/Orbit-PS3-0.9.5.pkg')
b=p.read_bytes()
assert b[:4]==b'\x7fPKG'
assert struct.unpack_from('>I',b,4)[0]==0x80000001
assert struct.unpack_from('>Q',b,24)[0]==len(b)
assert b[48:84].rstrip(b'\x00')==('UP0001-'+title+'_00-0000000000000000').encode()
e=Path('build/audio-probe.elf' if probe else 'build/orbit.elf').read_bytes()
assert e[:6]==b'\x7fELF\x02\x02', 'expected 64-bit big-endian ELF'
assert struct.unpack_from('>H',e,18)[0]==21, 'expected PowerPC64'
s=(stage/'USRDIR/EBOOT.BIN').read_bytes()
assert s[:4]==b'SCE\x00'
f=(stage/'PARAM.SFO').read_bytes()
assert f[:4]==b'\x00PSF'
_,keys,data,count=struct.unpack_from('<4I',f,4)
values={}
for i in range(count):
    keyoff,fmt,n,capacity,offset=struct.unpack_from('<HHIII',f,20+16*i)
    name=f[keys+keyoff:].split(b'\x00')[0].decode()
    raw=f[data+offset:data+offset+n]
    values[name]=raw.rstrip(b'\x00').decode() if fmt==0x0204 else raw.hex()
assert values['TITLE_ID']==title
assert values['APP_VER']==('00.20' if probe else '00.95')
assert not (stage/'USRDIR/data').exists(), 'never ship user data or server keys'
assert (stage/'ICON0.PNG').read_bytes()[:8]==b'\x89PNG\r\n\x1a\n'
if True:
    # Check the import tables after sprxlinker has filled their counts.
    linked = Path('build/audio-probe-stripped.elf' if probe else 'build/orbit-stripped.elf').read_bytes()
    shoff = struct.unpack_from('>Q', linked, 40)[0]
    entsize, nsections, strings = struct.unpack_from('>HHH', linked, 58)
    headers = [struct.unpack_from('>IIQQQQIIQQ', linked, shoff+i*entsize) for i in range(nsections)]
    table = headers[strings]
    names = linked[table[4]:table[4]+table[5]]
    sections = {names[h[0]:].split(b'\0')[0].decode(): h for h in headers}
    def at(address, size):
        for h in headers:
            if h[1] != 8 and h[3] <= address and address+size <= h[3]+h[5]:
                offset = h[4] + address-h[3]
                return linked[offset:offset+size]
        raise AssertionError('unmapped import address')
    imports = sections['.lib.stub']
    found = set()
    for offset in range(imports[4], imports[4]+imports[5], 44):
        n = struct.unpack_from('>H', linked, offset+6)[0]
        name, fnids, stubs = struct.unpack_from('>III', linked, offset+16)
        lib = at(name, 10).split(b'\0')[0]
        if lib == b'cellMic':
            assert n == 9 and len(at(fnids, n*4)) == 36 and len(at(stubs, n*4)) == 36
        found.add(lib)
    assert b'cellMic' in found, 'cellMic import must survive final linking'
    if not probe:
        assert b'cellAudio' in found, 'cellAudio import (voice playback) missing'
    print('PASS: finalized import tables: ' + ', '.join(sorted(x.decode() for x in found)))
print(('Audio probe: ' if probe else '')+'PASS: finalized PKG header/size/content ID, PowerPC64 big-endian ELF, SELF magic, SFO title/version, icon, no bundled user secrets')
print('SHA256',hashlib.sha256(b).hexdigest())
