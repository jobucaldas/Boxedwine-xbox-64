/* Internal generated-thunk ABI. All marshalling runs inside the host boundary. */
#ifndef D3DPASS_RUNTIME_H
#define D3DPASS_RUNTIME_H
#define COBJMACROS
#include "d3dpass.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
struct generated_params { unsigned method; void *args; };
#include "../generated/packets.h"
NTSTATUS d3dpass_generated_call(unsigned method, void *args);
NTSTATUS d3dpass_host_generated(void *args);
NTSTATUS d3dpass_marshal_error(void);
void *d3dpass_unwrap(const void *obj);
void *d3dpass_wrap_iid(uintptr_t obj, REFIID iid);
int d3dpass_take_wrap_error(void);
const void *d3dpass_vtable(REFIID iid);
ULONG d3dpass_reference(void *obj, int delta);
#endif
