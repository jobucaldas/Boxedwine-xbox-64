from pathlib import Path
import re
root=Path('x64/on-device')
headers='\n'.join(p.read_text() for p in Path('x64/uwp-host/include/uwph').glob('*.h'))
headers=re.sub(r'/\*.*?\*/','',headers,flags=re.S)
items=[]; wrappers=[]
names=Path('x64/uwp-host/harness/shim.map').read_text().split('global:')[1].split('local:')[0].replace(';',' ').split()
# Fixed signatures are generated from the public uwph declarations; varargs and
# callbacks require explicit bridging and stay visible as unresolved in W1.
for ret,name,args in re.findall(r'UWPH_API\s+([^;{}]*?)\buwph_(\w+)\s*\(([^;]*?)\)\s*;',headers,re.S):
 if name not in names and name!='__errno_location': continue
 ret=' '.join(ret.split()); args=' '.join(args.split())
 if '...' in args or 'va_list' in args or '(*' in args: continue
 params=[]
 for a in args.split(','):
  if a.strip() in ('void',''): continue
  m=re.search(r'(\w+)\s*(?:\[[^]]*\])?$',a.strip())
  if not m: raise Exception(a)
  params.append(m[1])
 wrappers.append(f'static {ret} SYSV a_{name}({args}) {{ '+('' if ret=='void' else 'return ')+f'uwph_{name}('+','.join(params)+'); }')
 items.append((name,'a_'+name))
(root/'elf_adapters.inc').write_text('/* Fixed-signature SysV -> Windows ABI adapters. See gen-elf-adapters.py. */\n'+'\n'.join(wrappers)+'\nstatic const struct provider host_symbols[] = {\n'+''.join(f'{{"{n}",(void *){v}}},\n' for n,v in items)+'{NULL,NULL}};\n')
print('adapters',len(items))
