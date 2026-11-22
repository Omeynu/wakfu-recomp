import struct,sys,uuid
HDR=bytes.fromhex('3026b2758e66cf11a6d900aa0062ce6c')
SP=uuid.UUID('B7DC0791-A9B7-11CF-8EE6-00C00C205365').bytes_le
FP=uuid.UUID('8CABDCA1-A947-11CF-8EE4-00C00C205365').bytes_le
VID=uuid.UUID('BC19EFC0-5B4D-11CF-A8FD-00805F5C442B').bytes_le
AUD=uuid.UUID('F8699E40-5B4D-11CF-A8FD-00805F5C442B').bytes_le
def parse(d,base,label):
    size,=struct.unpack('<Q',d[base+16:base+24]); n,=struct.unpack('<I',d[base+24:base+28])
    o=base+30; out=[]
    for _ in range(n):
        g=d[o:o+16]; s,=struct.unpack('<Q',d[o+16:o+24])
        if g==FP:
            dur,=struct.unpack('<Q',d[o+64:o+72]); pre,=struct.unpack('<Q',d[o+80:o+88]); fsz,=struct.unpack('<Q',d[o+40:o+48])
            mx,=struct.unpack('<I',d[o+100:o+104]); br,=struct.unpack('<I',d[o+100+4:o+108])
            out.append('durée %.1fs taille %d paquet %d débit %d'%(dur/1e7-pre/1e3,fsz,mx,br))
        if g==SP:
            st=d[o+24:o+40]; tl,=struct.unpack('<I',d[o+64:o+68]); t=d[o+78:o+78+tl]
            if st==VID:
                w,h=struct.unpack('<II',t[0:8]); fcc=t[11+16:11+20]
                out.append('vidéo %dx%d %s'%(w,h,fcc.decode('latin1')))
            elif st==AUD:
                tag,ch,sr,ab,ba,bps=struct.unpack('<HHIIHH',t[:16]); out.append('audio tag 0x%x %dch %dHz %dB/s'%(tag,ch,sr,ab))
        o+=s
    print(label,' | '.join(out))
for p in sys.argv[1:]:
    d=open(p,'rb').read(); i=0;k=0
    while True:
        i=d.find(HDR,i)
        if i<0: break
        try: parse(d,i,'%s@%x'%(p.split('/')[-1],i))
        except Exception as e: print(p,hex(i),'err',e)
        i+=16;k+=1
