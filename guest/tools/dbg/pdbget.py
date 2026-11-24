import struct,sys,os,urllib.request
def cv(path):
    b=open(path,'rb').read()
    pe=struct.unpack_from('<I',b,0x3c)[0]
    nsec=struct.unpack_from('<H',b,pe+6)[0]; opt=struct.unpack_from('<H',b,pe+20)[0]
    magic=struct.unpack_from('<H',b,pe+24)[0]
    dd=pe+24+(112 if magic==0x20b else 96)
    rva,sz=struct.unpack_from('<II',b,dd+6*8)
    sec=pe+24+opt
    off=None
    for i in range(nsec):
        s=sec+i*40
        vs,va,rs,rp=struct.unpack_from('<IIII',b,s+8)
        if va<=rva<va+max(vs,rs): off=rva-va+rp
    for k in range(sz//28):
        e=off+k*28
        t=struct.unpack_from('<I',b,e+12)[0]
        if t==2:
            ptr=struct.unpack_from('<I',b,e+24)[0]
            g=b[ptr+4:ptr+20]; age=struct.unpack_from('<I',b,ptr+20)[0]
            name=b[ptr+24:ptr+24+60].split(b'\0')[0].decode()
            d1,d2,d3=struct.unpack('<IHH',g[:8]); d4=g[8:].hex().upper()
            guid='%08X%04X%04X%s'%(d1,d2,d3,d4)
            return name,guid+str(age)
for f in sys.argv[1:]:
    r=cv(f)
    if not r: print(f,'no cv'); continue
    name,key=r; name=os.path.basename(name)
    out='/tmp/dwmre/sym/%s/%s'%(name,key); os.makedirs(out,exist_ok=True)
    url='https://msdl.microsoft.com/download/symbols/%s/%s/%s'%(name,key,name)
    try:
        req=urllib.request.Request(url,headers={'User-Agent':'Microsoft-Symbol-Server/10.0'})
        data=urllib.request.urlopen(req,timeout=60).read()
        open(out+'/'+name,'wb').write(data); print(f,name,key,len(data))
    except Exception as ex: print(f,name,key,'FAIL',ex)
