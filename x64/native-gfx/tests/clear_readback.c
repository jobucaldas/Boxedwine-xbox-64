/* Guest-role proof through the linked uwph static registry and O2 helper.
 * Wine loads this PE; host D3D11 here is Wine builtin, not Xbox D3D11. */
#define COBJMACROS
#include "d3dpass.h"
#include <stdio.h>
#define FIELD(t,o) (*(volatile uint64_t *)((char *)(t)+(o)))
#define TEB_ERROR(t) (*(volatile uint32_t *)((char *)(t)+UWPH_TEB_LASTERROR))
#define CHECK(call) do { hr=(call); if(FAILED(hr)) { failed=__LINE__; goto cleanup; } } while(0)
int main(void)
{
    void *teb=uwph_teb_current();
    struct uwph_host_teb_state host={0};
    struct uwph_teb_swap swap={0};
    ID3D11Device *device=NULL;
    ID3D11DeviceContext *context=NULL,*extra=NULL;
    ID3D11Texture2D *target=NULL,*staging=NULL;
    ID3D11RenderTargetView *view=NULL;
    IDXGIFactory1 *factory=NULL;
    HRESULT hr=S_OK; int failed=0, pixel_ok=0, state_ok=1;
    unsigned char pixel[4]={0};
    if(d3dpass_host_register()) return 1;
    host.peb=FIELD(teb,UWPH_TEB_PEB); host.tls_pointer=FIELD(teb,UWPH_TEB_TLSPOINTER);
    host.last_error=TEB_ERROR(teb);
    for(int i=0;i<64;++i) host.tls_slots[i]=FIELD(teb,0x1480+i*8);
    if(d3dpass_guest_init(teb,&host)) return 1;
    uwph_teb_capture_guest(teb,&swap);
    swap.guest[3]=0x23456000; swap.guest[4]=0x12345000;
    uwph_teb_enter_guest(teb,&swap);
    for(int i=0;i<64;++i) FIELD(teb,0x1480+i*8)=0x50000000u+i;
    TEB_ERROR(teb)=0x9876;
    CHECK(d3dpass_CreateDXGIFactory1(&IID_IDXGIFactory1,(void **)&factory));
    CHECK(d3dpass_D3D11CreateDevice(D3D_DRIVER_TYPE_HARDWARE,0,NULL,0,&device,NULL,&context));
    ID3D11Device_GetImmediateContext(device,&extra);
    if(!extra) { failed=__LINE__; goto cleanup; }
    ID3D11DeviceContext_AddRef(extra); ID3D11DeviceContext_Release(extra);
    D3D11_TEXTURE2D_DESC desc={.Width=64,.Height=64,.MipLevels=1,.ArraySize=1,.Format=DXGI_FORMAT_R8G8B8A8_UNORM,.SampleDesc={1,0},.Usage=D3D11_USAGE_DEFAULT,.BindFlags=D3D11_BIND_RENDER_TARGET};
    CHECK(ID3D11Device_CreateTexture2D(device,&desc,NULL,&target));
    CHECK(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource *)target,NULL,&view));
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(ID3D11Device_CreateTexture2D(device,&desc,NULL,&staging));
    const FLOAT color[4]={0,128.0f/255.0f,1,1};
    ID3D11DeviceContext_ClearRenderTargetView(context,view,color);
    ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)staging,(ID3D11Resource *)target);
    D3D11_MAPPED_SUBRESOURCE mapped={0};
    CHECK(ID3D11DeviceContext_Map(context,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&mapped));
    if(mapped.pData && mapped.RowPitch>=64*4) {
        for(int i=0;i<4;++i) pixel[i]=((const unsigned char *)mapped.pData)[i];
        pixel_ok=pixel[0]==0 && pixel[1]==128 && pixel[2]==255 && pixel[3]==255;
    }
    ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)staging,0);
cleanup:
    if(view) ID3D11RenderTargetView_Release(view);
    if(staging) ID3D11Texture2D_Release(staging);
    if(target) ID3D11Texture2D_Release(target);
    if(extra) ID3D11DeviceContext_Release(extra);
    if(context) ID3D11DeviceContext_Release(context);
    if(device) ID3D11Device_Release(device);
    if(factory) IDXGIFactory1_Release(factory);
    state_ok &= FIELD(teb,UWPH_TEB_PEB)==0x12345000;
    state_ok &= FIELD(teb,UWPH_TEB_TLSPOINTER)==0x23456000;
    state_ok &= TEB_ERROR(teb)==0x9876;
    for(int i=0;i<64;++i) { state_ok &= FIELD(teb,0x1480+i*8)==0x50000000u+i; FIELD(teb,0x1480+i*8)=host.tls_slots[i]; }
    uwph_teb_leave_guest(teb,&swap); TEB_ERROR(teb)=host.last_error;
    int ok=!failed && pixel_ok && state_ok && !d3dpass_stats.host_peb_bad && !d3dpass_stats.unimplemented && d3dpass_stats.calls>=19;
    puts("HOST d3d11 = Wine builtin stand-in; THROUGH-uwph d3dpass.so + uwph_host_call; Wine PE loader");
    printf("BOUNDARY calls=%u host_peb_bad=%u guest_restored=%d unsupported=%u failure_line=%d hr=0x%08lx\n",d3dpass_stats.calls,d3dpass_stats.host_peb_bad,state_ok,d3dpass_stats.unimplemented,failed,(unsigned long)hr);
    printf("READBACK %u,%u,%u,%u %s\n",pixel[0],pixel[1],pixel[2],pixel[3],ok?"PASS":"FAIL");
    return !ok;
}
