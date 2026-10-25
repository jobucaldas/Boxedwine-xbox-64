/*
 * d3dpass: guest D3D11 calls -> uwph boundary -> host d3d11.dll/dxgi.dll.
 *
 * Shaped like a Wine unixlib (unixlib.h): the host side is registered in the
 * uwph static registry as "d3dpass.so" and exports __wine_unix_call_funcs, a
 * table of NTSTATUS (*)(void *params) entries indexed by enum d3dpass_funcs.
 * Host COM objects cross the boundary only as opaque 64-bit handles; guest
 * code holds WRAPPER objects whose vtables enter the table through
 * uwph_host_call (DESIGN.md: never hand a raw host interface to guest code).
 *
 * Off-device the host d3d11/dxgi are Wine's builtin DLLs (stand-in).
 */
#ifndef D3DPASS_H
#define D3DPASS_H

#include <stdint.h>
#include <d3d11.h>
#include "uwph/uwph.h"

typedef uint64_t d3dpass_handle;          /* host IUnknown*, never called by guest */
typedef NTSTATUS (*d3dpass_entry_t)(void *params);

enum d3dpass_funcs {
    d3dpass_create_device,                /* D3D11CreateDevice */
    d3dpass_create_factory,               /* CreateDXGIFactory1 */
    d3dpass_unknown_addref,               /* IUnknown::AddRef */
    d3dpass_unknown_release,              /* IUnknown::Release */
    d3dpass_device_create_texture2d,      /* ID3D11Device::CreateTexture2D */
    d3dpass_device_create_rtv,            /* ID3D11Device::CreateRenderTargetView */
    d3dpass_device_get_immediate_context, /* ID3D11Device::GetImmediateContext */
    d3dpass_context_clear_rtv,            /* ID3D11DeviceContext::ClearRenderTargetView */
    d3dpass_context_copy_resource,        /* ID3D11DeviceContext::CopyResource */
    d3dpass_context_map,                  /* ID3D11DeviceContext::Map */
    d3dpass_context_unmap,                /* ID3D11DeviceContext::Unmap */
    d3dpass_funcs_count
};

/* Pointers inside params are guest memory, read/written by the host in-process. */
struct create_device_params {
    uint32_t driver_type, flags, level_count;
    const D3D_FEATURE_LEVEL *levels;
    d3dpass_handle device, context;       /* out */
    D3D_FEATURE_LEVEL level;              /* out */
    HRESULT hr;                           /* out */
};
struct create_factory_params { d3dpass_handle factory; HRESULT hr; };
struct unknown_params { d3dpass_handle obj; ULONG ref; };
struct create_texture2d_params {
    d3dpass_handle device;
    const D3D11_TEXTURE2D_DESC *desc;
    const D3D11_SUBRESOURCE_DATA *data;
    d3dpass_handle texture;               /* out */
    HRESULT hr;
};
struct create_rtv_params {
    d3dpass_handle device, resource;
    const D3D11_RENDER_TARGET_VIEW_DESC *desc;
    d3dpass_handle view;                  /* out */
    HRESULT hr;
};
struct get_immediate_context_params { d3dpass_handle device, context; };
struct clear_rtv_params { d3dpass_handle context, view; float color[4]; };
struct copy_resource_params { d3dpass_handle context, dst, src; };
struct map_params {
    d3dpass_handle context, resource;
    uint32_t subresource, map_type, map_flags;
    D3D11_MAPPED_SUBRESOURCE mapped;      /* out; pData points at host memory */
    HRESULT hr;
};
struct unmap_params { d3dpass_handle context, resource; uint32_t subresource; };

/* Host side (host.c): add "d3dpass.so" to the uwph static registry. */
int d3dpass_host_register(void);

/* Guest side (guest.c).
 * d3dpass_guest_init runs in host state: it resolves the table with
 * uwph_dlopen/uwph_dlsym and records the same-thread host TEB snapshot that
 * uwph_host_call restores around every table entry. */
int d3dpass_guest_init(void *teb, const struct uwph_host_teb_state *host);
NTSTATUS d3dpass_unix_call(enum d3dpass_funcs code, void *params);
HRESULT d3dpass_D3D11CreateDevice(D3D_DRIVER_TYPE type, UINT flags,
    const D3D_FEATURE_LEVEL *levels, UINT level_count,
    ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context);
HRESULT d3dpass_CreateDXGIFactory1(REFIID iid, void **factory);

struct d3dpass_stats {
    unsigned calls;            /* boundary crossings */
    unsigned host_peb_bad;     /* crossings where gs:0x60 was not the host PEB */
    unsigned unimplemented;    /* wrapper methods outside the O3 subset */
};
extern struct d3dpass_stats d3dpass_stats;

#endif
