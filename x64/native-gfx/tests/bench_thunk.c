/* Off-device comparison only. Both paths use the same Wine stand-in context;
 * the generated path enters our uwph registry/TEB helper for every COM call. */
#include "runtime.h"
#include <stdio.h>
#define FIELD(t,o) (*(volatile uint64_t *)((char *)(t)+(o)))
#define TEB_ERROR(t) (*(volatile uint32_t *)((char *)(t)+UWPH_TEB_LASTERROR))
#define ITERATIONS 1000000u
static void enter(void *teb, struct uwph_teb_swap *swap)
{
    uwph_teb_enter_guest(teb,swap);
    for(unsigned i=0;i<64;++i) FIELD(teb,0x1480+i*8)=0x50000000+i;
    TEB_ERROR(teb)=0x9876;
}
static int leave(void *teb, struct uwph_teb_swap *swap,
                 const struct uwph_host_teb_state *host)
{
    int ok=FIELD(teb,UWPH_TEB_PEB)==0x12345000 &&
           FIELD(teb,UWPH_TEB_TLSPOINTER)==0x23456000 && TEB_ERROR(teb)==0x9876;
    for(unsigned i=0;i<64;++i) {
        ok &= FIELD(teb,0x1480+i*8)==0x50000000+i;
        FIELD(teb,0x1480+i*8)=host->tls_slots[i];
    }
    uwph_teb_leave_guest(teb,swap); TEB_ERROR(teb)=host->last_error;
    return ok;
}
/* Exercise typed interface outputs, input/output interface arrays, IID outputs,
 * and the +1 surface before timing. All calls here run in synthetic guest state. */
static int generated_surface(ID3D11Device *device,ID3D11DeviceContext *context)
{
    ID3D11Device1 *device1=NULL;
    ID3D11DeviceContext1 *context1=NULL;
    ID3D11Buffer *buffer=NULL,*got[2]={NULL,NULL};
    IDXGIDevice *dxgi=NULL;
    IDXGIAdapter *adapter=NULL;
    int ok=SUCCEEDED(ID3D11Device_QueryInterface(device,&IID_ID3D11Device1,(void **)&device1));
    if(device1) {
        ID3D11Device1_GetImmediateContext1(device1,&context1);
        ok &= context1!=NULL;
    }
    D3D11_BUFFER_DESC desc={.ByteWidth=64,.Usage=D3D11_USAGE_DEFAULT,.BindFlags=D3D11_BIND_VERTEX_BUFFER};
    ok &= SUCCEEDED(ID3D11Device_CreateBuffer(device,&desc,NULL,&buffer));
    if(buffer) {
        ID3D11Buffer *buffers[2]={buffer,NULL};
        UINT strides[2]={16,0},offsets[2]={0,0};
        ID3D11DeviceContext_IASetVertexBuffers(context,0,2,buffers,strides,offsets);
        ID3D11DeviceContext_IAGetVertexBuffers(context,0,2,got,strides,offsets);
        ok &= got[0]==buffer && got[1]==NULL && strides[0]==16;
        buffers[0]=NULL;
        ID3D11DeviceContext_IASetVertexBuffers(context,0,2,buffers,strides,offsets);
    }
    ok &= SUCCEEDED(ID3D11Device_QueryInterface(device,&IID_IDXGIDevice,(void **)&dxgi));
    if(dxgi) ok &= SUCCEEDED(IDXGIDevice_GetAdapter(dxgi,&adapter));
    if(got[0]) ID3D11Buffer_Release(got[0]);
    if(got[1]) ID3D11Buffer_Release(got[1]);
    if(buffer) ID3D11Buffer_Release(buffer);
    if(adapter) IDXGIAdapter_Release(adapter);
    if(dxgi) IDXGIDevice_Release(dxgi);
    if(context1) ID3D11DeviceContext1_Release(context1);
    if(device1) ID3D11Device1_Release(device1);
    return ok;
}
int main(void)
{
    void *teb=uwph_teb_current();
    struct uwph_host_teb_state host={0};
    struct uwph_teb_swap swap={0};
    ID3D11Device *device=NULL;
    ID3D11DeviceContext *context=NULL;
    LARGE_INTEGER frequency,start,end;
    int ok=1;
    if(d3dpass_host_register()) return 1;
    host.peb=FIELD(teb,UWPH_TEB_PEB);
    host.tls_pointer=FIELD(teb,UWPH_TEB_TLSPOINTER);
    host.last_error=TEB_ERROR(teb);
    for(unsigned i=0;i<64;++i) host.tls_slots[i]=FIELD(teb,0x1480+i*8);
    if(d3dpass_guest_init(teb,&host)) return 1;
    uwph_teb_capture_guest(teb,&swap);
    swap.guest[3]=0x23456000; swap.guest[4]=0x12345000;
    enter(teb,&swap);
    HRESULT hr=d3dpass_D3D11CreateDevice(D3D_DRIVER_TYPE_HARDWARE,0,NULL,0,&device,NULL,&context);
    ok &= leave(teb,&swap,&host);
    if(FAILED(hr) || !device || !context) { printf("BENCH create failed hr=%08lx\n",(unsigned long)hr); return 1; }
    enter(teb,&swap);
    int surface_ok=generated_surface(device,context);
    ok &= surface_ok;
    ok &= leave(teb,&swap,&host);
    /* This private handle is used exclusively by the direct baseline with host
     * TEB restored. It is never returned as a guest COM interface. */
    ID3D11DeviceContext *direct=d3dpass_unwrap(context);
    for(unsigned i=0;i<10000;++i)
        ID3D11DeviceContext_IASetPrimitiveTopology(direct,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    enter(teb,&swap);
    for(unsigned i=0;i<10000;++i)
        ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ok &= leave(teb,&swap,&host);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for(unsigned i=0;i<ITERATIONS;++i)
        ID3D11DeviceContext_IASetPrimitiveTopology(direct,(i&1)?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    QueryPerformanceCounter(&end);
    double direct_ns=(double)(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/ITERATIONS;
    unsigned before=d3dpass_stats.calls;
    QueryPerformanceCounter(&start);
    enter(teb,&swap);
    for(unsigned i=0;i<ITERATIONS;++i)
        ID3D11DeviceContext_IASetPrimitiveTopology(context,(i&1)?D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ok &= leave(teb,&swap,&host);
    QueryPerformanceCounter(&end);
    double thunk_ns=(double)(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/ITERATIONS;
    unsigned measured=d3dpass_stats.calls-before;
    enter(teb,&swap);
    D3D11_PRIMITIVE_TOPOLOGY topology=0;
    ID3D11DeviceContext_IAGetPrimitiveTopology(context,&topology);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    ok &= leave(teb,&swap,&host);
    ok &= topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && measured==ITERATIONS &&
          !d3dpass_stats.host_peb_bad && !d3dpass_stats.unimplemented;
    puts("OFF-DEVICE ONLY: Wine builtin D3D11 stand-in; not Xbox performance");
    puts("THROUGH-uwph: Wine PE loader -> generated wrapper -> d3dpass.so registry -> uwph_host_call -> generated host stub");
    printf("GENERATED surface (+1, IID outputs, interface arrays) %s\n",surface_ok?"PASS":"FAIL");
    printf("THUNK_NS=%.3f DIRECT_NS=%.3f\n",thunk_ns,direct_ns);
    printf("BENCH iterations=%u crossings=%u host_peb_bad=%u guest_restored=%d %s\n",ITERATIONS,measured,d3dpass_stats.host_peb_bad,ok,ok?"PASS":"FAIL");
    return !ok;
}
