import struct, hashlib, sys
src,dst,addr=sys.argv[1],sys.argv[2],int(sys.argv[3],16)
full=bytearray(open(src,'rb').read()); base=0x20000
img=full[base:]
nseg=img[1]; off=24; segs=[]
for i in range(nseg):
    va,ln=struct.unpack_from('<II',img,off); segs.append((va,ln,off+8)); off+=8+ln
fo=[base+o+addr-va for va,ln,o in segs if va<=addr<va+ln][0]
assert full[fo:fo+2]==bytes.fromhex('9947'), full[fo:fo+2].hex()
full[fo:fo+2]=bytes.fromhex('c947')   # c.li a5,6 -> c.li a5,18 (SOC_MOD_CLK_XTAL)
# recompute checksum (after segments, padded so checksum is last byte of 16-byte block)
cs=0xEF
for va,ln,o in segs:
    for b in full[base+o:base+o+ln]: cs^=b
end=off; pad=15-(end%16); ckpos=base+end+pad
full[ckpos]=cs
hash_appended=img[23]
if hash_appended:
    full[ckpos+1:ckpos+33]=hashlib.sha256(full[base:ckpos+1]).digest()
open(dst,'wb').write(full)
print('patched at file offset',hex(fo),'checksum',hex(cs),'hash',bool(hash_appended))
