/* n13: documented WinRT ABI, independently declared from Windows metadata.
 * ABI reference: microsoft/windows-rs Windows/ApplicationModel/{Core,
 * ExtendedExecution}/mod.rs. No background execution guarantee is assumed. */
#include <asyncinfo.h>

typedef struct life_object {
    struct {
        IInspectableVtblX base;
        HRESULT (STDMETHODCALLTYPE *first)(void *, void **);
    } *lpVtbl;
} life_object;
typedef struct life_action {
    struct {
        IInspectableVtblX base;
        HRESULT (STDMETHODCALLTYPE *call)(void *);
    } *lpVtbl;
} life_action;
typedef struct {
    struct {
        IInspectableVtblX base;
        void *background_add, *background_remove, *leaving_add, *leaving_remove;
        HRESULT (STDMETHODCALLTYPE *entered_add)(void *, void *, INT64 *);
    } *lpVtbl;
} life_core2;
typedef struct {
    struct {
        IInspectableVtblX base;
        void *get_reason;
        HRESULT (STDMETHODCALLTYPE *put_reason)(void *, INT32);
        void *get_description, *put_description, *get_progress, *put_progress;
        void *add_revoked, *remove_revoked;
        HRESULT (STDMETHODCALLTYPE *request)(void *, void **);
    } *lpVtbl;
} life_session;
typedef struct {
    struct {
        IInspectableVtblX base;
        void *put_completed, *get_completed;
        HRESULT (STDMETHODCALLTYPE *results)(void *, INT32 *);
    } *lpVtbl;
} life_async;
static const GUID life_core2_iid =
    {0x998681fb,0x1ab6,0x4b7f,{0xbe,0x4a,0x9a,0x06,0x45,0x22,0x4c,0x04}};
static const GUID life_session_iid =
    {0xaf908a2d,0x118b,0x48f1,{0x93,0x08,0x0c,0x4f,0xc4,0x1e,0x20,0x0f}};
static const GUID life_closable_iid =
    {0x30d5a829,0x7fa4,0x4026,{0x83,0xbb,0xd7,0x5b,0xae,0x4e,0xa9,0x9e}};

static void life_log(const char *fmt, ...)
{
    SYSTEMTIME t;
    char body[700];
    va_list ap;
    GetSystemTime(&t);
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    /* out_line appends and fflush-es each complete record. */
    note("n13 | %04u-%02u-%02uT%02u:%02u:%02u.%03uZ tick_ms=%llu %s",
         t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,
         (unsigned long long)GetTickCount64(), body);
}

typedef struct life_handler life_handler;
struct life_handler {
    struct {
        HRESULT (STDMETHODCALLTYPE *qi)(void *, REFIID, void **);
        ULONG (STDMETHODCALLTYPE *addref)(void *);
        ULONG (STDMETHODCALLTYPE *release)(void *);
        HRESULT (STDMETHODCALLTYPE *invoke)(void *, void *, void *);
    } *lpVtbl;
    GUID iid;
    int kind;
};
static HRESULT STDMETHODCALLTYPE life_qi(void *self, REFIID iid, void **out)
{
    life_handler *h = self;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IAgileObject_) &&
        !IsEqualIID(iid, &h->iid)) return E_NOINTERFACE;
    *out = self;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE life_ref(void *self) { (void)self; return 2; }
static HRESULT STDMETHODCALLTYPE life_invoke(void *self, void *sender, void *args)
{
    (void)sender;
    life_handler *h = self;
    life_log("%s", h->kind == 0 ? "Suspending" : h->kind == 1 ? "Resuming" : "EnteredBackground");
    if (h->kind == 0 && args) {
        life_object *event = args, *op = NULL;
        life_action *deferral = NULL;
        HRESULT hr = event->lpVtbl->first(event, (void **)&op);
        if (SUCCEEDED(hr)) hr = op->lpVtbl->first(op, (void **)&deferral);
        ULONGLONG start = GetTickCount64();
        life_log("SuspendingDeferral GetDeferral hr=0x%08lx", hx(hr));
        if (deferral) {
            /* Complete immediately after flushing; never stretch the OS deadline. */
            hr = deferral->lpVtbl->call(deferral);
            life_log("SuspendingDeferral Complete hr=0x%08lx elapsed_ms=%llu", hx(hr),
                     (unsigned long long)(GetTickCount64() - start));
            deferral->lpVtbl->base.Release(deferral);
        }
        if (op) op->lpVtbl->base.Release(op);
    }
    return S_OK;
}
static struct {
    HRESULT (STDMETHODCALLTYPE *qi)(void *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *addref)(void *);
    ULONG (STDMETHODCALLTYPE *release)(void *);
    HRESULT (STDMETHODCALLTYPE *invoke)(void *, void *, void *);
} life_handler_vtbl = {life_qi, life_ref, life_ref, life_invoke};
/* Parameterized EventHandler<T> IIDs from WinRT signature UUIDs. Static
 * delegates remain alive for the application lifetime, including after n13. */
static life_handler life_handlers[] = {
    {(void *)&life_handler_vtbl,{0x338579bf,0x1a35,0x5cc4,{0xa6,0x22,0xa6,0xf3,0x84,0xfd,0x89,0x2c}},0},
    {(void *)&life_handler_vtbl,{0xc50898f6,0xc536,0x5f47,{0x85,0x83,0x8b,0x2c,0x24,0x38,0xa1,0x3b}},1},
    {(void *)&life_handler_vtbl,{0xe0739c32,0xfc14,0x5361,{0xa8,0xb3,0x08,0x09,0x69,0x9f,0xbc,0xbd}},2}
};

static void rung_n13(void)
{
    if (!g_packaged || !g_local_state || !g_dispatcher) {
        stage("n13", "lifecycle-log", "SKIP", "requires foreground UWP CoreWindow activation and LocalState; no off-device lifecycle proof");
        return;
    }
    HSTRING cls = NULL;
    ICoreApplication_ *app = NULL;
    life_core2 *app2 = NULL;
    INT64 tokens[3] = {0};
    const WCHAR *name = L"Windows.ApplicationModel.Core.CoreApplication";
    HRESULT hr = WindowsCreateString(name, (UINT32)wcslen(name), &cls);
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(cls, &IID_ICoreApplication_, (void **)&app);
    if (cls) WindowsDeleteString(cls);
    cls = NULL;
    life_log("CoreApplication factory hr=0x%08lx", hx(hr));
    if (app) {
        hr = app->lpVtbl->add_Suspending(app, &life_handlers[0], &tokens[0]);
        life_log("add_Suspending hr=0x%08lx", hx(hr));
        hr = app->lpVtbl->add_Resuming(app, &life_handlers[1], &tokens[1]);
        life_log("add_Resuming hr=0x%08lx", hx(hr));
        hr = app->lpVtbl->base.QueryInterface(app, &life_core2_iid, (void **)&app2);
        if (SUCCEEDED(hr)) hr = app2->lpVtbl->entered_add(app2, &life_handlers[2], &tokens[2]);
        life_log("add_EnteredBackground hr=0x%08lx", hx(hr));
    }
    /* Request once in the foreground before pumping the observation window. */
    name = L"Windows.ApplicationModel.ExtendedExecution.ExtendedExecutionSession";
    life_object *instance = NULL;
    life_session *session = NULL;
    life_async *async = NULL;
    IAsyncInfo *info = NULL;
    hr = WindowsCreateString(name, (UINT32)wcslen(name), &cls);
    if (SUCCEEDED(hr)) hr = RoActivateInstance(cls, (IInspectable **)&instance);
    if (SUCCEEDED(hr)) hr = instance->lpVtbl->base.QueryInterface(instance, &life_session_iid, (void **)&session);
    if (SUCCEEDED(hr)) hr = session->lpVtbl->put_reason(session, 0); /* Unspecified */
    if (SUCCEEDED(hr)) hr = session->lpVtbl->request(session, (void **)&async);
    life_log("ExtendedExecutionSession Reason=Unspecified RequestExtensionAsync hr=0x%08lx", hx(hr));
    if (async) {
        hr = async->lpVtbl->base.QueryInterface(async, &IID_IAsyncInfo, (void **)&info);
        life_log("ExtendedExecution IAsyncInfo hr=0x%08lx", hx(hr));
    }
    life_log("20 s observation window BEGIN: press Home once and return; send ladder-native.log");
    ULONGLONG start = GetTickCount64();
    int reported = !info;
    while (GetTickCount64() - start < 20000) {
        pump();
        if (!reported) {
            AsyncStatus status;
            hr = IAsyncInfo_get_Status(info, &status);
            if (FAILED(hr) || status != Started) {
                INT32 result = -1;
                if (SUCCEEDED(hr)) hr = async->lpVtbl->results(async, &result);
                life_log("ExtendedExecutionResult=%s value=%d hr=0x%08lx",
                         SUCCEEDED(hr) && result == 0 ? "Allowed" : SUCCEEDED(hr) && result == 1 ? "Denied" : "unavailable", result, hx(hr));
                reported = 1;
            }
        }
        Sleep(20);
    }
    if (!reported) {
        life_log("ExtendedExecutionResult=pending at window end; cancel once");
        hr = IAsyncInfo_Cancel(info);
        life_log("ExtendedExecution Cancel hr=0x%08lx", hx(hr));
    }
    if (info) IAsyncInfo_Release(info);
    if (async) async->lpVtbl->base.Release(async);
    if (session) {
        life_action *closable = NULL;
        hr = session->lpVtbl->base.QueryInterface(session, &life_closable_iid, (void **)&closable);
        if (SUCCEEDED(hr)) {
            hr = closable->lpVtbl->call(closable);
            closable->lpVtbl->base.Release(closable);
        }
        life_log("ExtendedExecutionSession Close hr=0x%08lx", hx(hr));
        session->lpVtbl->base.Release(session);
    }
    if (instance) instance->lpVtbl->base.Release(instance);
    if (cls) WindowsDeleteString(cls);
    if (app2) app2->lpVtbl->base.Release(app2);
    if (app) app->lpVtbl->base.Release(app);
    life_log("20 s observation window END; handlers remain registered");
    stage("n13", "lifecycle-log", "INFO", "informational only; inspect timestamped lifecycle/deferral/ExtendedExecution lines; no background execution guarantee");
}
