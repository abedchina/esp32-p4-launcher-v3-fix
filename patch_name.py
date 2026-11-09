# Replace the home-screen subtitle "ESP32-P4  Application Hub" with a custom text and fix checksum + SHA256
import struct, hashlib, sys
src,dst,text=sys.argv[1],sys.argv[2],sys.argv[3].encode()
old=b'ESP32-P4  Application Hub\x00'
full=bytearray(open(src,'rb').read()); base=0x20000
assert full.count(old)==1, full.count(old)
pos=full.index(old); assert len(text)<len(old)
full[pos:pos+len(old)]=text+b'\x00'*(len(old)-len(text))
img=full[base:]; nseg=img[1]; off=24; segs=[]
for i in range(nseg):
    va,ln=struct.unpack_from('<II',img,off); segs.append((va,ln,off+8)); off+=8+ln
assert any(base+o<=pos<base+o+ln for va,ln,o in segs)
cs=0xEF
for va,ln,o in segs:
    for b in full[base+o:base+o+ln]: cs^=b
pad=15-(off%16); ckpos=base+off+pad; full[ckpos]=cs
if img[23]: full[ckpos+1:ckpos+33]=hashlib.sha256(full[base:ckpos+1]).digest()
open(dst,'wb').write(full)
print('text at',hex(pos),'checksum',hex(cs))
