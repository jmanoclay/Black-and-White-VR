import re, struct, sys, hashlib
exe = sys.argv[1]; syms = sys.argv[2]
b = open(exe,'rb').read()
pe = struct.unpack_from('<I',b,0x3C)[0]; nsec = struct.unpack_from('<H',b,pe+6)[0]
ts = struct.unpack_from('<I',b,pe+8)[0]; optsz = struct.unpack_from('<H',b,pe+20)[0]
base = struct.unpack_from('<I',b,pe+24+28)[0]; so = pe+24+optsz
secs=[]
for i in range(nsec):
    o=so+40*i; name=b[o:o+8].rstrip(b'\0').decode(); vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8); secs.append((name,va,vs,rs,rp))
print('size',len(b),'sha1',hashlib.sha1(b).hexdigest(),'timestamp',ts)
print('sections:',', '.join(f'{n}@{va:#x}' for n,va,vs,rs,rp in secs))
def va2off(va):
    rva=va-base
    for n,sva,vs,rs,rp in secs:
        if sva<=rva<sva+rs: return rva-sva+rp
    return None
# RTTI type descriptors: ??_R0?AVName@@@8 = .data:0xADDR ; name string at +8
ok=bad=miss=0; bads=[]
for line in open(syms):
    m=re.match(r'\?\?_R0\?(AV|AU)(\S+?)@8 = \.\w+:0x([0-9A-F]+)',line)
    if not m: continue
    name='.?'+m.group(1)+m.group(2); off=va2off(int(m.group(3),16))
    if off is None: miss+=1; continue
    s=b[off+8:off+8+len(name)].decode('latin1')
    if s==name: ok+=1
    else: bad+=1; bads.append((name,s))
print(f'RTTI descriptors: {ok} match, {bad} mismatch, {miss} unmapped')
for x in bads[:5]: print('  mismatch',x)
# function entry sanity: count functions whose first byte is int3/zero padding (bad sign)
fn=0; pad=0
for line in open(syms):
    m=re.search(r'= \.text:0x([0-9A-F]+); // type:function',line)
    if not m: continue
    off=va2off(int(m.group(1),16))
    if off is None: continue
    fn+=1
    if b[off] in (0x00,0xCC): pad+=1
print(f'functions checked: {fn}, starting with 00/CC padding: {pad}')
