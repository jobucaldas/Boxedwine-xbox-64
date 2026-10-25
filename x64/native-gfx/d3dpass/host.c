#define COBJMACROS
#include "runtime.h"
#include "uwph/dl.h"
static NTSTATUS host_create_device(void *arg) { struct create_device_params *p=arg; ID3D11Device *d=NULL; ID3D11DeviceContext *c=NULL; p->hr=D3D11CreateDevice(NULL,p->driver_type,NULL,p->flags,p->levels,p->level_count,D3D11_SDK_VERSION,&d,&p->level,&c); p->device=(uintptr_t)d; p->context=(uintptr_t)c; return 0; }
static NTSTATUS host_create_factory(void *arg) { struct create_factory_params *p=arg; IDXGIFactory1 *f=NULL; p->hr=CreateDXGIFactory1(&IID_IDXGIFactory1,(void **)&f); p->factory=(uintptr_t)f; return 0; }
static NTSTATUS host_unknown_release(void *arg) { struct unknown_params *p=arg; p->ref=IUnknown_Release((IUnknown *)(uintptr_t)p->obj); return 0; }
const d3dpass_entry_t __wine_unix_call_funcs[d3dpass_funcs_count] = {
 [d3dpass_create_device]=host_create_device,
 [d3dpass_create_factory]=host_create_factory,
 [d3dpass_unknown_release]=host_unknown_release,
 [d3dpass_generated]=d3dpass_host_generated,
};
static const struct uwph_dl_symbol symbols[]={{"__wine_unix_call_funcs",(void *)__wine_unix_call_funcs},{NULL,NULL}};
static const struct uwph_dl_library library={"d3dpass.so",symbols,NULL,NULL};
int d3dpass_host_register(void) { return uwph_dl_register(&library); }
