"""Build-time only: combine read-only Ghidra exports with masked x64 patterns.
Capstone masks relative branch operands and RIP displacements. Object member
offsets, stack offsets, constants, opcodes and register encodings stay fixed.
The release executable contains these patterns; Python/Ghidra are not required.
"""
from pathlib import Path
import struct, json, os, argparse
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_JUMP, CS_GRP_CALL
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
ROOT=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--directory',type=Path,
 default=Path(os.environ.get('ProgramFiles',r'C:\Program Files'))/'RevvitySignalsSoftware'/'ChemDrawApplications_x64',
 help='Local installation containing the verified reference binaries')
INSTALL=parser.parse_args().directory
class PE:
 def __init__(self,path):
  self.b=path.read_bytes(); b=self.b; self.pe=self.u32(0x3c); self.opt=self.pe+24
  n=self.u16(self.pe+6); table=self.opt+self.u16(self.pe+20)
  self.sections=[struct.unpack_from('<IIII',b,table+i*40+8)+(self.u32(table+i*40+36),) for i in range(n)]
 def u16(self,p):return struct.unpack_from('<H',self.b,p)[0]
 def u32(self,p):return struct.unpack_from('<I',self.b,p)[0]
 def off(self,rva):
  for vs,va,rs,raw,flags in self.sections:
   if va<=rva<va+rs:return raw+rva-va
  if rva<self.u32(self.opt+60):return rva
  raise ValueError(hex(rva))
 def string(self,rva):
  p=self.off(rva);return self.b[p:self.b.index(b'\0',p)].decode('ascii')
 def imports(self):
  p=self.off(self.u32(self.opt+120));out={}
  while any(self.b[p:p+20]):
   ilt,_,_,name,iat=struct.unpack_from('<IIIII',self.b,p);dll=self.string(name)
   for i in range(100000):
    value=struct.unpack_from('<Q',self.b,self.off((ilt or iat)+8*i))[0]
    if not value:break
    if value>>63:continue
    out[iat+8*i]=(dll,self.string(value+2))
   p+=20
  return out
def masked(data,rva):
 mask=bytearray(b'\xff'*len(data));md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
 for ins in md.disasm(data,rva):
  start=ins.address-rva
  if ins.group(CS_GRP_JUMP) or ins.group(CS_GRP_CALL):
   for j in range(ins.imm_offset,ins.imm_offset+ins.imm_size):mask[start+j]=0
  if any(op.type==X86_OP_MEM and op.mem.base==X86_REG_RIP for op in ins.operands):
   for j in range(ins.disp_offset,ins.disp_offset+ins.disp_size):mask[start+j]=0
 return bytes(mask)
def matches(pe,data,mask):
 # Search via a longest fixed run, then validate all masked bytes.
 runs=[];start=None
 for i,m in enumerate(mask+b'\0'):
  if m==255 and start is None:start=i
  if m!=255 and start is not None:runs.append((start,i-start));start=None
 at,length=max(runs,key=lambda t:t[1]);anchor=data[at:at+length];out=[]
 for vs,va,rs,raw,flags in pe.sections:
  if not flags&0x20000000:continue
  section=pe.b[raw:raw+rs];p=0
  while True:
   hit=section.find(anchor,p)
   if hit<0:break
   p=hit+1;begin=hit-at
   if begin<0 or begin+len(data)>rs:continue
   if all((section[begin+i]&mask[i])==(data[i]&mask[i]) for i in range(len(data))):out.append(va+begin)
 return out
signatures=[];report=[]
for module in ('ChemDrawBase.dll','ChemDrawUI.dll'):
 pe=PE(INSTALL/module)
 for line in (ROOT/'ghidra'/f'{module}.targets.tsv').read_text(encoding='utf-8-sig').splitlines():
  mod,rva,source,name,prototype,hexdata=line.split('\t');rva=int(rva,16)
  if name=='MISSING':raise RuntimeError(f'Missing Ghidra function {module}+{rva:x}')
  data=bytes.fromhex(hexdata)[:128];anchor=0;mask=masked(data,rva)
  candidates=matches(pe,data,mask)
  if candidates!=[rva]:
   # Tiny accessors need adjacent context; do not accept ambiguous bodies.
   anchor=48;begin=rva-anchor;p=pe.off(begin);data=pe.b[p:p+176];mask=masked(data,begin)
   candidates=[v+anchor for v in matches(pe,data,mask)]
  if candidates!=[rva]:raise RuntimeError(f'Nonunique pattern: {module}+{rva:x}: {candidates}')
  signatures.append(f' {{L"{module}",0x{rva:x},{json.dumps(name)},{json.dumps(prototype)},{json.dumps(source)},{anchor},"{data.hex()}","{mask.hex()}"}}')
  report.append(dict(module=module,rva=f'0x{rva:x}',function=name,prototype=prototype,source=source,length=len(data),context_anchor=anchor))
(ROOT/'signatures.hpp').write_text('#pragma once\n#include "pe.hpp"\ninline const Signature signatures[]={\n'+',\n'.join(signatures)+'\n};\n')
(ROOT/'ghidra'/'signature-catalog.json').write_text(json.dumps(report,indent=2))
pe=PE(INSTALL/'ChemDraw.exe.before-latency-fix');imports=pe.imports();names=[]
for rva in (0x7f46a,0x7f4a6):
 p=pe.off(rva);assert pe.b[p:p+2]==b'\xff\x25';iat=rva+6+struct.unpack_from('<i',pe.b,p+2)[0]
 dll,name=imports[iat];assert dll.lower()=='chemdrawui.dll';names.append(name)
(ROOT/'bootstrap-names.hpp').write_text('#pragma once\ninline const char* idleImportNames[]={'+','.join(json.dumps(n) for n in names)+'};\n')
print(f'{len(signatures)} unique patterns; bootstrap imports: {names}')
