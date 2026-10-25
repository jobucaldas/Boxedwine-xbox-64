/* Minimal, same-thread O3 wrapper implementation. Unsupported slots are NULL;
 * callers must use only the documented subset, until O4 fills the vtables. */
#define COBJMACROS
#include "d3dpass.h"
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
/* A bounded pool avoids any guest CRT call while synthetic guest TLS is set.
 * Wrapper lifetime is independent of the host's internal COM references. */
struct wrapper { const void *vtbl; d3dpass_handle handle; ULONG refs; const IID *iid; };
static struct wrapper pool[64];
static d3dpass_handle handle(void *p) { return p ? ((struct wrapper *)p)->handle : 0; }
static void *wrap(d3dpass_handle h,const void *vt,const IID *iid)
{
    for(unsigned i=0;i<64;++i) if(!pool[i].refs) {
        pool[i]=(struct wrapper){vt,h,1,iid}; return &pool[i];
    }
    struct unknown_params p={h,0}; d3dpass_unix_call(d3dpass_unknown_release,&p); return NULL;
}
static ULONG STDMETHODCALLTYPE addref(void *obj)
{
    struct wrapper *w=obj; struct unknown_params p={w->handle,0};
    d3dpass_unix_call(d3dpass_unknown_addref,&p); return ++w->refs;
}
static ULONG STDMETHODCALLTYPE release(void *obj)
{
    struct wrapper *w=obj; struct unknown_params p={w->handle,0};
    d3dpass_unix_call(d3dpass_unknown_release,&p); return --w->refs;
}
static HRESULT STDMETHODCALLTYPE query(void *obj,REFIID iid,void **out)
{
    struct wrapper *w=obj;
    if(!out) return E_POINTER;
    *out=NULL;
    if(IsEqualGUID(iid,&IID_IUnknown)||IsEqualGUID(iid,w->iid)) { addref(obj); *out=obj; return S_OK; }
    ++d3dpass_stats.unimplemented; return E_NOINTERFACE;
}
/* Shared first three COM slots have identical x64 signatures. */
#define UNKNOWN(T) .QueryInterface=(HRESULT (STDMETHODCALLTYPE *)(T *,REFIID,void **))query, .AddRef=(ULONG (STDMETHODCALLTYPE *)(T *))addref, .Release=(ULONG (STDMETHODCALLTYPE *)(T *))release
static const ID3D11Texture2DVtbl texture_vt={UNKNOWN(ID3D11Texture2D)};
static const ID3D11RenderTargetViewVtbl view_vt={UNKNOWN(ID3D11RenderTargetView)};
static const IDXGIFactory1Vtbl factory_vt={UNKNOWN(IDXGIFactory1)};
static const ID3D11DeviceContextVtbl context_vt;
static HRESULT STDMETHODCALLTYPE texture(ID3D11Device *d,const D3D11_TEXTURE2D_DESC *desc,const D3D11_SUBRESOURCE_DATA *data,ID3D11Texture2D **out)
{
    if(!out) return E_POINTER;
    *out=NULL; struct create_texture2d_params p={handle(d),desc,data,0,E_FAIL};
    d3dpass_unix_call(d3dpass_device_create_texture2d,&p);
    if(SUCCEEDED(p.hr)) { *out=wrap(p.texture,&texture_vt,&IID_ID3D11Texture2D); if(!*out) return E_OUTOFMEMORY; } return p.hr;
}
static HRESULT STDMETHODCALLTYPE rtv(ID3D11Device *d,ID3D11Resource *r,const D3D11_RENDER_TARGET_VIEW_DESC *desc,ID3D11RenderTargetView **out)
{
    if(!out) return E_POINTER;
    *out=NULL; struct create_rtv_params p={handle(d),handle(r),desc,0,E_FAIL};
    d3dpass_unix_call(d3dpass_device_create_rtv,&p);
    if(SUCCEEDED(p.hr)) { *out=wrap(p.view,&view_vt,&IID_ID3D11RenderTargetView); if(!*out) return E_OUTOFMEMORY; } return p.hr;
}
static void STDMETHODCALLTYPE immediate(ID3D11Device *d,ID3D11DeviceContext **out)
{
    if(!out) return;
    struct get_immediate_context_params p={handle(d),0}; d3dpass_unix_call(d3dpass_device_get_immediate_context,&p);
    *out=wrap(p.context,&context_vt,&IID_ID3D11DeviceContext);
}
static void STDMETHODCALLTYPE clear(ID3D11DeviceContext *c,ID3D11RenderTargetView *v,const FLOAT color[4])
{
    struct clear_rtv_params p={handle(c),handle(v),{color[0],color[1],color[2],color[3]}};
    d3dpass_unix_call(d3dpass_context_clear_rtv,&p);
}
static void STDMETHODCALLTYPE copy(ID3D11DeviceContext *c,ID3D11Resource *dst,ID3D11Resource *src)
{
    struct copy_resource_params p={handle(c),handle(dst),handle(src)}; d3dpass_unix_call(d3dpass_context_copy_resource,&p);
}
static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext *c,ID3D11Resource *r,UINT sub,D3D11_MAP type,UINT flags,D3D11_MAPPED_SUBRESOURCE *out)
{
    if(!out) return E_POINTER;
    struct map_params p={.context=handle(c),.resource=handle(r),.subresource=sub,.map_type=type,.map_flags=flags,.hr=E_FAIL};
    d3dpass_unix_call(d3dpass_context_map,&p); *out=p.mapped; return p.hr;
}
static void STDMETHODCALLTYPE unmap(ID3D11DeviceContext *c,ID3D11Resource *r,UINT sub)
{
    struct unmap_params p={handle(c),handle(r),sub}; d3dpass_unix_call(d3dpass_context_unmap,&p);
}
static const ID3D11DeviceContextVtbl context_vt={UNKNOWN(ID3D11DeviceContext),.ClearRenderTargetView=clear,.CopyResource=copy,.Map=map,.Unmap=unmap};
static const ID3D11DeviceVtbl device_vt={UNKNOWN(ID3D11Device),.CreateTexture2D=texture,.CreateRenderTargetView=rtv,.GetImmediateContext=immediate};
HRESULT d3dpass_D3D11CreateDevice(D3D_DRIVER_TYPE type,UINT flags,const D3D_FEATURE_LEVEL *levels,UINT count,ID3D11Device **device,D3D_FEATURE_LEVEL *level,ID3D11DeviceContext **context)
{
    if(!device) return E_POINTER;
    *device=NULL; if(context) *context=NULL;
    struct create_device_params p={.driver_type=type,.flags=flags,.level_count=count,.levels=levels,.hr=E_FAIL};
    d3dpass_unix_call(d3dpass_create_device,&p);
    if(FAILED(p.hr)) return p.hr;
    *device=wrap(p.device,&device_vt,&IID_ID3D11Device);
    ID3D11DeviceContext *c=wrap(p.context,&context_vt,&IID_ID3D11DeviceContext);
    if(level) *level=p.level;
    if(!*device || !c) { if(*device) release(*device); if(c) release(c); *device=NULL; return E_OUTOFMEMORY; }
    if(context) *context=c; else release(c);
    return p.hr;
}
HRESULT d3dpass_CreateDXGIFactory1(REFIID iid,void **out)
{
    if(!out) return E_POINTER;
    *out=NULL;
    if(!IsEqualGUID(iid,&IID_IDXGIFactory1)) return E_NOINTERFACE;
    struct create_factory_params p={0,E_FAIL}; d3dpass_unix_call(d3dpass_create_factory,&p);
    if(SUCCEEDED(p.hr)) { *out=wrap(p.factory,&factory_vt,&IID_IDXGIFactory1); if(!*out) return E_OUTOFMEMORY; } return p.hr;
}
