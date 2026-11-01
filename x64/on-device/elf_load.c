/* In-process ELF64 W1 loader. All executable pages follow RW -> RX FromApp.
 * System ELF dependencies are never handed to the Windows DLL loader.
 * TLS relocations are censused and left deferred; constructors are not run. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include "uwph/uwph.h"
#include "elf_load.h"
#define SYSV __attribute__((sysv_abi))
typedef struct { unsigned char ident[16]; uint16_t type,machine; uint32_t version; uint64_t entry,phoff,shoff; uint32_t flags; uint16_t ehsize,phentsize,phnum,shentsize,shnum,shstrndx; } Ehdr;
typedef struct { uint32_t type,flags; uint64_t offset,vaddr,paddr,filesz,memsz,align; } Phdr;
typedef struct { uint32_t name,type; uint64_t flags,addr,offset,size; uint32_t link,info; uint64_t align,entsize; } Shdr;
typedef struct { uint32_t name; unsigned char info,other; uint16_t shndx; uint64_t value,size; } Sym;
typedef struct { uint64_t offset,info; int64_t addend; } Rela;
struct provider { const char *name; void *addr; };
#include "elf_adapters.inc"
#define WRAP(r,n,args,call) static r SYSV c_##n args { return n call; }
WRAP(void *,memcpy,(void *a,const void *b,size_t n),(a,b,n))
WRAP(void *,memmove,(void *a,const void *b,size_t n),(a,b,n))
WRAP(void *,memset,(void *a,int b,size_t n),(a,b,n))
WRAP(int,memcmp,(const void *a,const void *b,size_t n),(a,b,n))
WRAP(size_t,strlen,(const char *a),(a))
WRAP(int,strcmp,(const char *a,const char *b),(a,b))
WRAP(int,strncmp,(const char *a,const char *b,size_t n),(a,b,n))
WRAP(char *,strcpy,(char *a,const char *b),(a,b))
WRAP(char *,strncpy,(char *a,const char *b,size_t n),(a,b,n))
WRAP(char *,strchr,(const char *a,int b),(a,b))
WRAP(char *,strrchr,(const char *a,int b),(a,b))
WRAP(char *,strstr,(const char *a,const char *b),(a,b))
WRAP(int,tolower,(int a),(a))
WRAP(int,toupper,(int a),(a))
WRAP(int,isalpha,(int a),(a))
WRAP(int,isdigit,(int a),(a))
WRAP(double,sqrt,(double a),(a))
WRAP(double,sin,(double a),(a))
WRAP(double,cos,(double a),(a))
WRAP(double,floor,(double a),(a))
WRAP(double,ceil,(double a),(a))
WRAP(double,pow,(double a,double b),(a,b))
#define P(n) {#n,(void *)c_##n}
static const struct provider libc_symbols[]={P(memcpy),P(memmove),P(memset),P(memcmp),P(strlen),P(strcmp),P(strncmp),P(strcpy),P(strncpy),P(strchr),P(strrchr),P(strstr),P(tolower),P(toupper),P(isalpha),P(isdigit),P(sqrt),P(sin),P(cos),P(floor),P(ceil),P(pow),{NULL,NULL}};
struct image { unsigned char *file,*base; size_t size,span; Ehdr *eh; Phdr *ph; Shdr *sh; Sym *sym; size_t nsym; const char *str; size_t strsize; };
static struct image images[2];
static FILE *census;
static void (*logger)(const char *);
static unsigned counts[4],tls_count;
static unsigned char *stubs;
static unsigned nstubs;
static char stub_names[512][192];
static void report(const char *fmt,...)
{
 char b[768]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof(b),fmt,ap); va_end(ap);
 if(census){fprintf(census,"%s\n",b);fflush(census);} if(logger)logger(b);
}
static int inside(uint64_t off,uint64_t n,uint64_t total){return off<=total && n<=total-off;}
static const char *symname(struct image *m,Sym *s)
{ return s->name<m->strsize && memchr(m->str+s->name,0,m->strsize-s->name)?m->str+s->name:NULL; }
static void *lookup(struct image *m,const char *name)
{
 for(size_t i=0;i<m->nsym;i++){Sym *s=m->sym+i;const char *n=symname(m,s);
  if(n && s->shndx && (s->info>>4) && !(s->other&3) && !strcmp(n,name)){
   if((s->info&15)==10 || (s->info&15)==6)return NULL; /* IFUNC/TLS not callable in W1 */
   if(s->shndx==0xfff1)return (void *)(uintptr_t)s->value;
   return inside(s->value,1,m->span)?m->base+s->value:NULL;
  }
 }return NULL;
}
static int64_t SYSV missing(unsigned index)
{ uwph_errno=UWPH_ENOSYS;report("unresolved-call %s -> -1/ENOSYS",stub_names[index]);return -1; }
static void *resolve(const char *name,int current)
{
 void *p=NULL;for(int i=0;i<current;i++)if((p=lookup(images+i,name))){counts[0]++;return p;}
 const struct provider *tables[]={host_symbols,libc_symbols};
 for(int t=0;t<2;t++)for(const struct provider *s=tables[t];s->name;s++)if(!strcmp(s->name,name)){counts[t+1]++;return s->addr;}
 for(unsigned i=0;i<nstubs;i++)if(!strcmp(name,stub_names[i])){counts[3]++;return stubs+i*32;}
 if(nstubs==512 || strlen(name)>=sizeof(stub_names[0]))return NULL;
 unsigned i=nstubs++;strcpy(stub_names[i],name);unsigned char *b=stubs+i*32;
 /* mov edi,index; movabs rax,SysV logger; jmp rax. No argument/stack access. */
 b[0]=0xbf;memcpy(b+1,&i,4);b[5]=0x48;b[6]=0xb8;p=(void *)missing;memcpy(b+7,&p,8);b[15]=0xff;b[16]=0xe0;
 report("unresolved %s",name);counts[3]++;return b;
}
static int load_image(struct image *m,const wchar_t *path,int index)
{
 FILE *f=_wfopen(path,L"rb");if(!f){report("FAIL open %ls",path);return 0;}
 if(fseek(f,0,SEEK_END)||ftell(f)<0){fclose(f);return 0;}m->size=(size_t)ftell(f);rewind(f);
 m->file=malloc(m->size);if(!m->file){fclose(f);return 0;}size_t got=fread(m->file,1,m->size,f);fclose(f);
 if(got!=m->size||m->size<sizeof(Ehdr))return 0;m->eh=(Ehdr *)m->file;Ehdr *e=m->eh;
 if(memcmp(e->ident,"\177ELF\2\1\1",7)||e->type!=3||e->machine!=62||e->phentsize!=sizeof(Phdr)||e->shentsize!=sizeof(Shdr)||!inside(e->phoff,(uint64_t)e->phnum*sizeof(Phdr),m->size)||!inside(e->shoff,(uint64_t)e->shnum*sizeof(Shdr),m->size))return 0;
 m->ph=(Phdr *)(m->file+e->phoff);m->sh=(Shdr *)(m->file+e->shoff);
 for(unsigned i=0;i<e->phnum;i++){Phdr *p=m->ph+i;if(p->type!=1)continue;
  if(p->filesz>p->memsz||!inside(p->offset,p->filesz,m->size)||!inside(p->vaddr,p->memsz,256*1024*1024)||((p->flags&3)==3))return 0;
  if(p->vaddr+p->memsz>m->span)m->span=(size_t)(p->vaddr+p->memsz);
 }
 m->span=(m->span+4095)&~(size_t)4095;if(!m->span)return 0;
 m->base=VirtualAllocFromApp(NULL,m->span,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!m->base)return 0;
 for(unsigned i=0;i<e->phnum;i++){Phdr *p=m->ph+i;if(p->type==1)memcpy(m->base+p->vaddr,m->file+p->offset,(size_t)p->filesz);}
 for(unsigned i=0;i<e->shnum;i++){Shdr *s=m->sh+i;if(s->type!=11)continue;
  if(s->entsize!=sizeof(Sym)||s->link>=e->shnum||!inside(s->offset,s->size,m->size))return 0;
  Shdr *str=m->sh+s->link;if(!inside(str->offset,str->size,m->size))return 0;
  m->sym=(Sym *)(m->file+s->offset);m->nsym=(size_t)(s->size/sizeof(Sym));m->str=(char *)m->file+str->offset;m->strsize=(size_t)str->size;
 }
 if(!m->sym)return 0;
 for(unsigned i=0;i<e->shnum;i++){Shdr *s=m->sh+i;if(s->type!=4||!(s->flags&2))continue;
  if(s->entsize!=sizeof(Rela)||s->size%sizeof(Rela)||!inside(s->offset,s->size,m->size))return 0;
  for(size_t j=0;j<s->size/sizeof(Rela);j++){
   Rela *r=(Rela *)(m->file+s->offset)+j;unsigned type=(unsigned)r->info;uint64_t si=r->info>>32,value=0;
   if(!inside(r->offset,8,m->span)||si>=m->nsym)return 0;
   Sym *sym=m->sym+si;const char *name=symname(m,sym);if(!name)return 0;
   if(type==16||type==17||type==18){tls_count++;report("TLS deferred type=%u symbol=%s offset=%llx",type,name,(unsigned long long)r->offset);continue;}
   if(type==0)continue;
   if(type==8)value=(uintptr_t)m->base+r->addend;
   else if(type==1||type==6||type==7){
    if(sym->shndx){if(!inside(sym->value,1,m->span))return 0;value=(uintptr_t)(m->base+sym->value);}
    else {void *addr=resolve(name,index);if(!addr)return 0;value=(uintptr_t)addr;}
    value+=r->addend;
   }else{report("FAIL unsupported relocation %u",type);return 0;}
   memcpy(m->base+r->offset,&value,8);
  }
 }
 DWORD old;
 /* Protect each page once from the union of PT_LOAD flags; reject shared W+X. */
 for(size_t page=0;page<m->span;page+=4096){unsigned flags=0;
  for(unsigned i=0;i<e->phnum;i++){Phdr *p=m->ph+i;if(p->type==1&&page<p->vaddr+p->memsz&&page+4096>p->vaddr)flags|=p->flags;}
  if((flags&3)==3)return 0;
  DWORD prot=(flags&1)?PAGE_EXECUTE_READ:(flags&2)?PAGE_READWRITE:(flags&4)?PAGE_READONLY:PAGE_NOACCESS;
  if(!VirtualProtectFromApp(m->base+page,4096,prot,&old))return 0;
 }
 for(unsigned i=0;i<e->phnum;i++){Phdr *p=m->ph+i;if(p->type!=0x6474e552)continue;
  uint64_t start=p->vaddr&~4095ull,end=(p->vaddr+p->memsz)&~4095ull;
  if(!inside(start,end-start,m->span)|| (end>start&&!VirtualProtectFromApp(m->base+start,(size_t)(end-start),PAGE_READONLY,&old)))return 0;
 }
 FlushInstructionCache(GetCurrentProcess(),m->base,m->span);report("mapped %ls bytes=%llu RW->RX/R/RW RELRO",path,(unsigned long long)m->span);return 1;
}
int elf_load_test(const wchar_t *directory,const wchar_t *state,void (*log)(const char *))
{
 wchar_t path[1024];int ok=0;logger=log;memset(counts,0,sizeof(counts));tls_count=nstubs=0;
 swprintf(path,1024,L"%ls\\elf-census.txt",state);census=_wfopen(path,L"w");if(!census)return 1;
 stubs=VirtualAllocFromApp(NULL,512*32,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!stubs)goto done;
 const wchar_t *files[]={L"ntdll.so",L"win32u.so"};
 for(int i=0;i<2;i++){swprintf(path,1024,L"%ls\\%ls",directory,files[i]);if(!load_image(images+i,path,i))goto done;}
 DWORD old;if(!VirtualProtectFromApp(stubs,512*32,PAGE_EXECUTE_READ,&old))goto done;
 FlushInstructionCache(GetCurrentProcess(),stubs,512*32);
 /* ntdll_wcsicmp needs the case tables that init_unix_codepage sets up (NULL before init -> AV);
  * RtlUTF8ToUnicodeN is table-free, so it is the W1 call target. */
 typedef LONG (SYSV *utf8_fn)(uint16_t *,DWORD,DWORD *,const char *,DWORD);
 utf8_fn conv=(utf8_fn)lookup(images,"RtlUTF8ToUnicodeN");
 uint16_t out[16]={0};DWORD res=0;const char in[]="Wine \xc3\xa9\xe2\x82\xac";
 LONG st=conv?conv(out,sizeof(out),&res,in,(DWORD)strlen(in)):-1;
 ok=conv&&st==0&&res==7*2&&out[0]=='W'&&out[5]==0xe9&&out[6]==0x20ac;
 report("call RtlUTF8ToUnicodeN status=0x%lx bytes=%lu u+%04x u+%04x",(unsigned long)st,(unsigned long)res,out[5],out[6]);
 report("elf-call=%s export=RtlUTF8ToUnicodeN (Wine ntdll.so code executed)",ok?"PASS":"FAIL");
 done:
 report("resolved ELF=%u uwph=%u libc=%u unresolved-relocations=%u unique-unresolved=%u TLS=%u (deferred; no initializers)",counts[0],counts[1],counts[2],counts[3],nstubs,tls_count);
 report("[w1] elf-load: %s",ok?"PASS":"FAIL");
 for(int i=0;i<2;i++){if(images[i].base)VirtualFree(images[i].base,0,MEM_RELEASE);free(images[i].file);memset(images+i,0,sizeof(images[i]));}
 if(stubs)VirtualFree(stubs,0,MEM_RELEASE);stubs=NULL;fclose(census);census=NULL;return ok?0:1;
}
