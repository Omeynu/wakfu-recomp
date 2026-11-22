import struct,sys,collections
for p in sys.argv[1:]:
    d=open(p,'rb').read()
    sig=d[:4]; be = sig==b'DNBW'
    e='>' if be else '<'
    ver,hver=struct.unpack(e+'II',d[4:12])
    segs=[struct.unpack(e+'II',d[12+8*i:20+8*i]) for i in range(5)]
    bd_off,bd_len=segs[0]; md_off,md_len=segs[1]
    flags,count=struct.unpack(e+'II',d[bd_off:bd_off+8])
    name=d[bd_off+8:bd_off+8+64].split(b'\0')[0].decode()
    entsz,=struct.unpack(e+'I',d[bd_off+8+64:bd_off+76])
    compact=flags&0x20000
    fmts=collections.Counter()
    if not compact:
        for i in range(count):
            o=md_off+i*entsz
            fd,=struct.unpack(e+'I',d[o:o+4]); f,=struct.unpack(e+'I',d[o+4:o+8])
            tag=f&3; ch=(f>>2)&7; sr=(f>>5)&0x3FFFF
            fmts[(['PCM','XMA','ADPCM','WMA'][tag],ch,sr)]+=1
    print(p.split('/')[-1],'v',ver,name,count,'compact' if compact else '',dict(fmts))
# détail
import os
p=os.environ.get('DETAIL')
if p:
    d=open(p,'rb').read(); e='>'
    segs=[struct.unpack(e+'II',d[12+8*i:20+8*i]) for i in range(5)]
    bd_off=segs[0][0]; md_off=segs[1][0]; wd_off=segs[4][0]
    flags,count=struct.unpack(e+'II',d[bd_off:bd_off+8]); entsz,=struct.unpack(e+'I',d[bd_off+72:bd_off+76])
    for i in range(min(count,int(os.environ.get('N','6')))):
        o=md_off+i*entsz
        fd,f,po,pl,lo,ll=struct.unpack(e+'6I',d[o:o+24])
        tag=f&3; ch=(f>>2)&7; sr=(f>>5)&0x3FFFF; ba=(f>>23)&0xff; bps=f>>31
        a=wd_off+po
        print(i,['PCM','XMA','ADPCM','WMA'][tag],ch,sr,'ba',ba,'bps',bps,'dur',fd>>4,'len',pl, d[a:a+16].hex())
