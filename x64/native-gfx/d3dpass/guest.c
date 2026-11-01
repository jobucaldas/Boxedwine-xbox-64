/* Same-thread wrapper pool and uwph dispatch for generated COM thunks. */
#define COBJMACROS
#include "runtime.h"
#include "uwph/dl.h"
#include <string.h>
struct d3dpass_stats d3dpass_stats;
static void *thread_teb;
static struct uwph_host_teb_state host_state;
static const d3dpass_entry_t *table;
struct dispatch { enum d3dpass_funcs code; void *params; };
static uintptr_t dispatch(void *arg)
{
    struct dispatch *d=arg;
    uintptr_t peb;
    __asm__ volatile("movq %%gs:0x60,%0":"=r"(peb));
    ++d3dpass_stats.calls;
    if(peb!=host_state.peb) { ++d3dpass_stats.host_peb_bad; return (uintptr_t)0xc0000001; }
    return table[d->code](d->params);
}
int d3dpass_guest_init(void *teb,const struct uwph_host_teb_state *host)
{
    void *lib=uwph_dlopen("d3dpass.so",0);
    if(!lib || !(table=uwph_dlsym(lib,"__wine_unix_call_funcs"))) return -1;
    thread_teb=teb; host_state=*host; return 0;
}
NTSTATUS d3dpass_unix_call(enum d3dpass_funcs code,void *params)
{
    struct dispatch d={code,params};
    if(!table || (unsigned)code>=d3dpass_funcs_count) return (NTSTATUS)0xc000000d;
    return (NTSTATUS)uwph_host_call(thread_teb,&host_state,dispatch,&d);
}
/* Same-thread bounded pool: no guest CRT/TLS access during synthetic state. */
struct wrapper { const void *vtbl; d3dpass_handle handle; ULONG refs; const IID *iid; };
static struct wrapper pool[512];
static int wrap_error;
void *d3dpass_unwrap(const void *obj)
{
    return obj ? (void *)(uintptr_t)((const struct wrapper *)obj)->handle : NULL;
}
NTSTATUS d3dpass_marshal_error(void)
{
    ++d3dpass_stats.unimplemented;
    return (NTSTATUS)0xc000000d;
}
int d3dpass_take_wrap_error(void) { int error=wrap_error; wrap_error=0; return error; }
void *d3dpass_wrap_iid(uintptr_t h,REFIID iid)
{
    if(!h) return NULL;
    const void *vt=d3dpass_vtable(iid);
    for(unsigned i=0;i<512;++i)
        if(pool[i].refs && pool[i].handle==h &&
           (pool[i].vtbl==vt || IsEqualGUID(iid,&IID_IUnknown))) {
            ++pool[i].refs; return &pool[i];
        }
    if(vt) for(unsigned i=0;i<512;++i) if(!pool[i].refs) {
        pool[i]=(struct wrapper){vt,h,1,iid}; return &pool[i];
    }
    /* Wrapping also occurs after device creation in guest state, so failed
     * adoption releases the host reference through the boundary. */
    struct unknown_params p={h,0};
    d3dpass_unix_call(d3dpass_unknown_release,&p); wrap_error=1; return NULL;
}
ULONG d3dpass_reference(void *obj,int delta)
{
    struct wrapper *w=obj;
    w->refs+=delta;
    return w->refs;
}
NTSTATUS d3dpass_generated_call(unsigned method,void *args)
{
    wrap_error=0;
    struct generated_params p={method,args};
    return d3dpass_unix_call(d3dpass_generated,&p);
}
HRESULT d3dpass_D3D11CreateDevice(D3D_DRIVER_TYPE type,UINT flags,const D3D_FEATURE_LEVEL *levels,UINT count,ID3D11Device **device,D3D_FEATURE_LEVEL *level,ID3D11DeviceContext **context)
{
    if(!device) return E_POINTER;
    *device=NULL; if(context) *context=NULL;
    struct create_device_params p={.driver_type=type,.flags=flags,.level_count=count,.levels=levels,.hr=E_FAIL};
    d3dpass_unix_call(d3dpass_create_device,&p);
    if(FAILED(p.hr)) return p.hr;
    *device=d3dpass_wrap_iid(p.device,&IID_ID3D11Device);
    ID3D11DeviceContext *c=d3dpass_wrap_iid(p.context,&IID_ID3D11DeviceContext);
    if(level) *level=p.level;
    if(!*device || !c) { if(*device) IUnknown_Release((IUnknown *)*device); if(c) IUnknown_Release((IUnknown *)c); *device=NULL; return E_OUTOFMEMORY; }
    if(context) *context=c; else IUnknown_Release((IUnknown *)c);
    return p.hr;
}
HRESULT d3dpass_CreateDXGIFactory1(REFIID iid,void **out)
{
    if(!out) return E_POINTER;
    *out=NULL;
    if(!IsEqualGUID(iid,&IID_IDXGIFactory1)) return E_NOINTERFACE;
    struct create_factory_params p={0,E_FAIL}; d3dpass_unix_call(d3dpass_create_factory,&p);
    if(SUCCEEDED(p.hr)) { *out=d3dpass_wrap_iid(p.factory,&IID_IDXGIFactory1); if(!*out) return E_OUTOFMEMORY; } return p.hr;
}
