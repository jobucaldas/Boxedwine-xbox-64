#define COBJMACROS
#include "d3dpass.h"
#include "uwph/dl.h"
static NTSTATUS host_create_device(void *arg) { struct create_device_params *p=arg; ID3D11Device *d=NULL; ID3D11DeviceContext *c=NULL; p->hr=D3D11CreateDevice(NULL,p->driver_type,NULL,p->flags,p->levels,p->level_count,D3D11_SDK_VERSION,&d,&p->level,&c); p->device=(uintptr_t)d; p->context=(uintptr_t)c; return 0; }
static NTSTATUS host_create_factory(void *arg) { struct create_factory_params *p=arg; IDXGIFactory1 *f=NULL; p->hr=CreateDXGIFactory1(&IID_IDXGIFactory1,(void **)&f); p->factory=(uintptr_t)f; return 0; }
static NTSTATUS host_unknown_addref(void *arg) { struct unknown_params *p=arg; p->ref=IUnknown_AddRef((IUnknown *)(uintptr_t)p->obj); return 0; }
static NTSTATUS host_unknown_release(void *arg) { struct unknown_params *p=arg; p->ref=IUnknown_Release((IUnknown *)(uintptr_t)p->obj); return 0; }
static NTSTATUS host_device_create_texture2d(void *arg) { struct create_texture2d_params *p=arg; ID3D11Texture2D *t=NULL; p->hr=ID3D11Device_CreateTexture2D((ID3D11Device *)(uintptr_t)p->device,p->desc,p->data,&t); p->texture=(uintptr_t)t; return 0; }
static NTSTATUS host_device_create_rtv(void *arg) { struct create_rtv_params *p=arg; ID3D11RenderTargetView *v=NULL; p->hr=ID3D11Device_CreateRenderTargetView((ID3D11Device *)(uintptr_t)p->device,(ID3D11Resource *)(uintptr_t)p->resource,p->desc,&v); p->view=(uintptr_t)v; return 0; }
static NTSTATUS host_device_get_immediate_context(void *arg) { struct get_immediate_context_params *p=arg; ID3D11DeviceContext *c=NULL; ID3D11Device_GetImmediateContext((ID3D11Device *)(uintptr_t)p->device,&c); p->context=(uintptr_t)c; return 0; }
static NTSTATUS host_context_clear_rtv(void *arg) { struct clear_rtv_params *p=arg; ID3D11DeviceContext_ClearRenderTargetView((ID3D11DeviceContext *)(uintptr_t)p->context,(ID3D11RenderTargetView *)(uintptr_t)p->view,p->color); return 0; }
static NTSTATUS host_context_copy_resource(void *arg) { struct copy_resource_params *p=arg; ID3D11DeviceContext_CopyResource((ID3D11DeviceContext *)(uintptr_t)p->context,(ID3D11Resource *)(uintptr_t)p->dst,(ID3D11Resource *)(uintptr_t)p->src); return 0; }
static NTSTATUS host_context_map(void *arg) { struct map_params *p=arg; p->hr=ID3D11DeviceContext_Map((ID3D11DeviceContext *)(uintptr_t)p->context,(ID3D11Resource *)(uintptr_t)p->resource,p->subresource,p->map_type,p->map_flags,&p->mapped); return 0; }
static NTSTATUS host_context_unmap(void *arg) { struct unmap_params *p=arg; ID3D11DeviceContext_Unmap((ID3D11DeviceContext *)(uintptr_t)p->context,(ID3D11Resource *)(uintptr_t)p->resource,p->subresource); return 0; }
const d3dpass_entry_t __wine_unix_call_funcs[d3dpass_funcs_count] = {
 [d3dpass_create_device]=host_create_device,
 [d3dpass_create_factory]=host_create_factory,
 [d3dpass_unknown_addref]=host_unknown_addref,
 [d3dpass_unknown_release]=host_unknown_release,
 [d3dpass_device_create_texture2d]=host_device_create_texture2d,
 [d3dpass_device_create_rtv]=host_device_create_rtv,
 [d3dpass_device_get_immediate_context]=host_device_get_immediate_context,
 [d3dpass_context_clear_rtv]=host_context_clear_rtv,
 [d3dpass_context_copy_resource]=host_context_copy_resource,
 [d3dpass_context_map]=host_context_map,
 [d3dpass_context_unmap]=host_context_unmap,
};
static const struct uwph_dl_symbol symbols[]={{"__wine_unix_call_funcs",(void *)__wine_unix_call_funcs},{NULL,NULL}};
static const struct uwph_dl_library library={"d3dpass.so",symbols,NULL,NULL};
int d3dpass_host_register(void) { return uwph_dl_register(&library); }
