/* Stock-Wine transport/chain diagnostic; no credentials or TLS exceptions. */
#define _WIN32_WINNT 0x0602
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void chain(HINTERNET r, const wchar_t *host) {
    PCCERT_CONTEXT cert = NULL; DWORD n = sizeof cert;
    if (!WinHttpQueryOption(r, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &n)) {
        printf("chain_query_err=%lu\n", GetLastError()); return;
    }
    CERT_CHAIN_PARA para = {0}; PCCERT_CHAIN_CONTEXT cc = NULL;
    para.cbSize = sizeof para;
    BOOL ok = CertGetCertificateChain(NULL, cert, NULL, cert->hCertStore, &para, 0, NULL, &cc);
    printf("chain_ok=%d chain_error_bits=0x%08lx err=%lu\n", ok, cc ? cc->TrustStatus.dwErrorStatus : 0xffffffffUL, ok ? 0 : GetLastError());
    if (cc) {
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl = {0}; CERT_CHAIN_POLICY_PARA pp = {0}; CERT_CHAIN_POLICY_STATUS ps = {0};
        ssl.cbSize=sizeof ssl; ssl.dwAuthType=AUTHTYPE_SERVER; ssl.pwszServerName=(wchar_t *)host;
        pp.cbSize=sizeof pp; pp.pvExtraPolicyPara=&ssl; ps.cbSize=sizeof ps;
        ok=CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,cc,&pp,&ps);
        printf("chain_ssl_policy_ok=%d error=0x%08lx\n",ok,ps.dwError);
        CertFreeCertificateChain(cc);
    }
    CertFreeCertificateContext(cert);
}
static HINTERNET request(HINTERNET s, const wchar_t *host, const wchar_t *path, int ws, HINTERNET *c, DWORD *status, DWORD *err) {
    *status=0; *err=0; *c=WinHttpConnect(s,host,443,0);
    HINTERNET r=*c ? WinHttpOpenRequest(*c,L"GET",path,NULL,NULL,NULL,WINHTTP_FLAG_SECURE) : NULL;
    if (!r || (ws && !WinHttpSetOption(r,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,NULL,0)) ||
        !WinHttpSendRequest(r,NULL,0,NULL,0,0,0) || !WinHttpReceiveResponse(r,NULL)) { *err=GetLastError(); return r; }
    DWORD n=sizeof *status;
    if (!WinHttpQueryHeaders(r,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,status,&n,NULL)) *err=GetLastError();
    chain(r,host); return r;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0); WSADATA wd; WSAStartup(MAKEWORD(2,2),&wd);
    ULONG size=0; ULONG e=GetAdaptersAddresses(AF_UNSPEC,0,NULL,NULL,&size);
    void *ad=malloc(size); if (ad) e=GetAdaptersAddresses(AF_UNSPEC,0,NULL,ad,&size);
    printf("adapters_status=%lu bytes=%lu\n",e,size); free(ad);
    struct addrinfo hints={0}, *ai=NULL; hints.ai_socktype=SOCK_STREAM;
    int ge=getaddrinfo("api.steampowered.com","443",&hints,&ai);
    printf("dns_err=%d\n",ge);
    for (struct addrinfo *p=ai;p;p=p->ai_next) { char h[128]; if (!getnameinfo(p->ai_addr,p->ai_addrlen,h,sizeof h,NULL,0,NI_NUMERICHOST)) printf("dns_family=%d address=%s\n",p->ai_family,h); }
    if(ai) freeaddrinfo(ai);
    HINTERNET s=WinHttpOpen(L"B70-https-probe/1",WINHTTP_ACCESS_TYPE_NO_PROXY,NULL,NULL,0), c=NULL;
    if(s) WinHttpSetTimeouts(s,10000,10000,15000,15000);
    DWORD status=0,err=0; HINTERNET r=request(s,L"api.steampowered.com",L"/ISteamDirectory/GetCMListForConnect/v1/?cellid=0",0,&c,&status,&err);
    printf("winhttp_status=%lu err=%lu\n",status,err);
    char body[131072]={0}; DWORD used=0,n=0;
    if(status==200) while(used<sizeof(body)-1 && WinHttpReadData(r,body+used,sizeof(body)-1-used,&n) && n) used+=n;
    printf("directory_bytes=%lu\n",used);
    char host[256]={0};
    /* Match an endpoint and type within the same JSON object, tolerating whitespace. */
    for(char *p=body;(p=strstr(p,"\"endpoint\""));p++) {
        char *end=strchr(p,'}'), *type=strstr(p,"websockets"), *v=strchr(p,':');
        if(!end || !type || type>end || !v) continue;
        v++; while(*v==' '||*v=='\t'||*v=='\r'||*v=='\n') v++; if(*v++!='"') continue;
        char *q=strchr(v,'"'); if(!q || q-v>=256 || q-v<5 || strncmp(q-4,":443",4)) continue;
        memcpy(host,v,q-v-4); break;
    }
    if(r) WinHttpCloseHandle(r);
    if(c) WinHttpCloseHandle(c);
    int upgraded=0; err=ERROR_NOT_FOUND; status=0;
    if(*host) {
        wchar_t wh[256]; MultiByteToWideChar(CP_UTF8,0,host,-1,wh,256);
        printf("wss_host=%s port=443\n",host);
        r=request(s,wh,L"/cmsocket/",1,&c,&status,&err);
        if(status==101) { HINTERNET w=WinHttpWebSocketCompleteUpgrade(r,0); if(w) { upgraded=1; err=0; WinHttpWebSocketClose(w,1000,NULL,0); WinHttpCloseHandle(w); } else err=GetLastError(); }
        if(r) WinHttpCloseHandle(r);
    if(c) WinHttpCloseHandle(c);
    }
    printf("wss_status=%lu\nwss_upgrade=%d err=%lu\n",status,upgraded,err);
    if(s) WinHttpCloseHandle(s);
    WSACleanup(); return 0;
}
