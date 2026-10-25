/* ---- n14: picked folder (USB) write/read/delete, observe only ----
 * Only from the "Boxedwine Pick USB folder" tiles (EntryPoint
 * Boxedwine.PickFolder / Boxedwine.PickFolderBigFile): FolderPicker ->
 * StorageApplicationPermissions.FutureAccessList.Add -> CreateFile2FromAppW
 * and DeleteFileFromAppW on the picked path. broadFileSystemAccess is not
 * used (not supported on Xbox). A refusal is logged and n14 stops there.
 * Interfaces absent from mingw-w64 are declared here at ABI level; slot
 * order and IIDs follow the Windows SDK windows.storage.pickers.h and
 * windows.storage.accesscache.h. */
#include <asyncinfo.h>
static int g_pick_folder;
static const GUID iid_folder_picker =
    {0x084f7799,0xf3fb,0x400a,{0x99,0xb1,0x7b,0x4a,0x77,0x2f,0xd6,0x0d}};
static const GUID iid_permissions =
    {0x4391dfaa,0xd033,0x48f9,{0x80,0x60,0x3e,0xc8,0x47,0xd2,0xe3,0xf1}};
typedef struct {
    struct {
        IInspectableVtblX base;
        void *get_ViewMode, *put_ViewMode, *get_SettingsIdentifier, *put_SettingsIdentifier;
        void *get_SuggestedStartLocation, *put_SuggestedStartLocation;
        void *get_CommitButtonText, *put_CommitButtonText;
        HRESULT (STDMETHODCALLTYPE *get_FileTypeFilter)(void *, __FIVector_1_HSTRING **);
        HRESULT (STDMETHODCALLTYPE *PickSingleFolderAsync)(void *, __FIAsyncOperation_1_Windows__CStorage__CStorageFolder **);
    } *lpVtbl;
} folder_picker_t;
typedef struct {
    struct {
        IInspectableVtblX base;
        /* IStorageItemAccessList::Add(file) -> token; first slot. */
        HRESULT (STDMETHODCALLTYPE *Add)(void *, __x_ABI_CWindows_CStorage_CIStorageItem *, HSTRING *);
    } *lpVtbl;
} access_list_t;
typedef struct {
    struct {
        IInspectableVtblX base;
        HRESULT (STDMETHODCALLTYPE *get_FutureAccessList)(void *, access_list_t **);
    } *lpVtbl;
} permissions_t;

typedef HANDLE (WINAPI *create2_from_app_fn)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
typedef BOOL (WINAPI *delete_from_app_fn)(LPCWSTR);

static FARPROC from_app_proc(const char *name)
{
    FARPROC p = sym(L"api-ms-win-core-file-fromapp-l1-1-0.dll", name);
    return p ? p : kernel_proc(name);
}

/* One file: CREATE_NEW (never touches an existing file), write, flush,
 * size, read back + compare, close, DeleteFileFromAppW. 1 = all OK. */
static int picked_file(const WCHAR *root, UINT64 total, const char **why)
{
    create2_from_app_fn create = (create2_from_app_fn)(void *)from_app_proc("CreateFile2FromAppW");
    delete_from_app_fn remove = (delete_from_app_fn)(void *)from_app_proc("DeleteFileFromAppW");
    const UINT64 block = 4 * MB;
    WCHAR path[MAX_PATH + 64], id[40];
    GUID unique;
    if (!create || !remove) {
        note("n14 | CreateFile2FromAppW=%s DeleteFileFromAppW=%s err=127",
             create ? "present" : "MISSING", remove ? "present" : "MISSING");
        *why = "REFUSED";
        return 0;
    }
    if (FAILED(CoCreateGuid(&unique)) || !StringFromGUID2(&unique, id, 40) ||
        _snwprintf(path, MAX_PATH + 64, L"%ls%lsboxedwine-n14-%llu-%ls.bin", root,
                   root[wcslen(root) - 1] == L'\\' ? L"" : L"\\",
                   (unsigned long long)(total / MB), id) < 0) {
        note("n14 | file name for %llu MiB not built (path too long?) err=%u",
             (unsigned long long)(total / MB), ERROR_FILENAME_EXCED_RANGE);
        *why = "FAIL";
        return 0;
    }
    note("n14 | file path=%ls target=%llu bytes (%llu MiB)", path,
         (unsigned long long)total, (unsigned long long)(total / MB));
    CREATEFILE2_EXTENDED_PARAMETERS ep = {sizeof(ep), FILE_ATTRIBUTE_NORMAL,
                                          FILE_FLAG_SEQUENTIAL_SCAN, 0, NULL, NULL};
    HANDLE h = create(path, GENERIC_READ | GENERIC_WRITE, 0, CREATE_NEW, &ep);
    if (h == INVALID_HANDLE_VALUE) {
        note("n14 | CreateFile2FromAppW CREATE_NEW REFUSED err=%lu; nothing created, no retry", GetLastError());
        *why = "REFUSED";
        return 0;
    }
    unsigned char *buf = VirtualAlloc(NULL, block, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    UINT64 written = 0, verified = 0;
    DWORD werr = buf ? 0 : GetLastError(), ferr = 0, rerr = 0, io;
    double t0 = now_ms();
    for (UINT64 i = 0; buf && written < total; i++) {
        memset(buf, (int)(i * 31 + 7) & 0xff, block);
        memcpy(buf, &i, sizeof(i));
        if (!WriteFile(h, buf, (DWORD)block, &io, NULL) || io != block) {
            werr = GetLastError();
            break;
        }
        written += io;
        if ((i & 15) == 0) pump();
    }
    double tw = now_ms() - t0;
    if (!FlushFileBuffers(h)) ferr = GetLastError();
    LARGE_INTEGER size = {0}, zero = {0};
    GetFileSizeEx(h, &size);
    SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
    t0 = now_ms();
    for (UINT64 i = 0; buf && verified < written; i++) {
        UINT64 tag;
        if (!ReadFile(h, buf, (DWORD)block, &io, NULL) || io != block) {
            rerr = GetLastError();
            break;
        }
        memcpy(&tag, buf, sizeof(tag));
        if (tag != i || buf[block - 1] != (unsigned char)((i * 31 + 7) & 0xff)) {
            rerr = ERROR_CRC;
            break;
        }
        verified += io;
        if ((i & 15) == 0) pump();
    }
    double tr = now_ms() - t0;
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    DWORD cerr = CloseHandle(h) ? 0 : GetLastError();
    DWORD derr = remove(path) ? 0 : GetLastError();
    int ok = written == total && verified == total && (UINT64)size.QuadPart == total &&
             !ferr && !cerr && !derr;
    note("n14 | %llu MiB %s: written=%llu bytes (err=%lu, %.0f ms) flush err=%lu size=%llu bytes "
         "verified=%llu bytes (err=%lu, %.0f ms) close err=%lu DeleteFileFromAppW %s err=%lu",
         (unsigned long long)(total / MB), ok ? "OK" : "FAILED",
         (unsigned long long)written, werr, tw, ferr, (unsigned long long)size.QuadPart,
         (unsigned long long)verified, rerr, tr, cerr, derr ? "FAILED (file may remain)" : "OK", derr);
    *why = ok ? "PASS" : "FAIL";
    return ok;
}

static void rung_n14(void)
{
    if (!g_pick_folder) {
        stage("n14", "picked-folder", "SKIP", "launch the \"Boxedwine Pick USB folder\" tile (EntryPoint Boxedwine.PickFolder)");
        return;
    }
    if (!g_device || !g_packaged || !g_local_state || !g_dispatcher) {
        stage("n14", "picked-folder", "SKIP", "requires verified Xbox UWP CoreWindow activation and LocalState; no off-device picker stand-in");
        return;
    }
    folder_picker_t *picker = NULL;
    IInspectable *instance = NULL;
    __FIVector_1_HSTRING *filter = NULL;
    __FIAsyncOperation_1_Windows__CStorage__CStorageFolder *op = NULL;
    IAsyncInfo *info = NULL;
    __x_ABI_CWindows_CStorage_CIStorageFolder *folder = NULL;
    __x_ABI_CWindows_CStorage_CIStorageItem *item = NULL;
    permissions_t *permissions = NULL;
    access_list_t *list = NULL;
    HSTRING cls = NULL, perm = NULL, star = NULL, path = NULL, token = NULL;
    static const WCHAR picker_cls[] = L"Windows.Storage.Pickers.FolderPicker";
    static const WCHAR perm_cls[] = L"Windows.Storage.AccessCache.StorageApplicationPermissions";
    const char *step = "RoActivateInstance FolderPicker";
    AsyncStatus status = Started;
    note("n14 | opening FolderPicker: pick (or create) a folder on the USB drive");
    HRESULT hr = WindowsCreateString(picker_cls, (UINT32)wcslen(picker_cls), &cls);
    if (SUCCEEDED(hr)) hr = RoActivateInstance(cls, &instance);
    if (SUCCEEDED(hr)) hr = IInspectable_QueryInterface(instance, &iid_folder_picker, (void **)&picker);
    if (SUCCEEDED(hr)) { step = "FileTypeFilter.Append(\"*\")"; hr = picker->lpVtbl->get_FileTypeFilter(picker, &filter); }
    if (SUCCEEDED(hr)) hr = WindowsCreateString(L"*", 1, &star);
    if (SUCCEEDED(hr)) hr = __FIVector_1_HSTRING_Append(filter, star);
    if (SUCCEEDED(hr)) { step = "PickSingleFolderAsync"; hr = picker->lpVtbl->PickSingleFolderAsync(picker, &op); }
    if (SUCCEEDED(hr)) hr = __FIAsyncOperation_1_Windows__CStorage__CStorageFolder_QueryInterface(op, &IID_IAsyncInfo, (void **)&info);
    double t0 = now_ms();
    while (SUCCEEDED(hr) && status == Started) {
        hr = IAsyncInfo_get_Status(info, &status);
        pump();
        Sleep(10);
    }
    if (SUCCEEDED(hr) && status == Error) {
        HRESULT error = E_FAIL;
        IAsyncInfo_get_ErrorCode(info, &error);
        hr = FAILED(error) ? error : E_FAIL;
    }
    if (SUCCEEDED(hr) && status == Canceled) hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (SUCCEEDED(hr)) hr = __FIAsyncOperation_1_Windows__CStorage__CStorageFolder_GetResults(op, &folder);
    if (info) note("n14 | PickSingleFolderAsync status=%d hr=0x%08lx after %.0f ms", (int)status, hx(hr), now_ms() - t0);
    if (SUCCEEDED(hr) && !folder) {
        stage("n14", "picked-folder", "SKIP", "FolderPicker cancelled (no folder); nothing created");
        goto cleanup;
    }
    if (SUCCEEDED(hr)) { step = "StorageFolder Path"; hr = __x_ABI_CWindows_CStorage_CIStorageFolder_QueryInterface(folder, &IID___x_ABI_CWindows_CStorage_CIStorageItem, (void **)&item); }
    if (SUCCEEDED(hr)) hr = __x_ABI_CWindows_CStorage_CIStorageItem_get_Path(item, &path);
    if (SUCCEEDED(hr)) { step = "FutureAccessList.Add"; hr = WindowsCreateString(perm_cls, (UINT32)wcslen(perm_cls), &perm); }
    if (SUCCEEDED(hr)) hr = RoGetActivationFactory(perm, &iid_permissions, (void **)&permissions);
    if (SUCCEEDED(hr)) hr = permissions->lpVtbl->get_FutureAccessList(permissions, &list);
    if (SUCCEEDED(hr)) hr = list->lpVtbl->Add(list, item, &token);
    if (FAILED(hr)) {
        stage("n14", "picked-folder", "REFUSED", "%s hr=0x%08lx; stopped, no other access path tried", step, hx(hr));
        goto cleanup;
    }
    const WCHAR *root = WindowsGetStringRawBuffer(path, NULL);
    note("n14 | picked folder path=%ls FutureAccessList token=%ls (entry kept for later runs)",
         root, WindowsGetStringRawBuffer(token, NULL));
    if (!*root) {
        stage("n14", "picked-folder", "REFUSED", "picked folder has no file-system path (err=%u); FromApp APIs need one", ERROR_BAD_PATHNAME);
        goto cleanup;
    }
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExW(root, &avail, NULL, NULL))
        note("n14 | free space in picked folder: %llu MiB", (unsigned long long)(avail.QuadPart / MB));
    else
        note("n14 | GetDiskFreeSpaceExW on picked folder err=%lu (informational)", GetLastError());
    const char *result = "FAIL";
    int ok64 = picked_file(root, 64 * MB, &result), okbig = 0;
    if (ok64 && g_big_file)
        okbig = picked_file(root, 2 * 1024 * MB + 256 * MB, &result);
    else if (ok64)
        note("n14 | 2304 MiB file SKIP: not requested (use the \"Boxedwine USB big file opt-in\" tile or --big-file)");
    stage("n14", "picked-folder", result, "path=%ls 64 MiB %s; 2304 MiB %s; see the '# n14 |' lines",
          root, ok64 ? "OK" : "FAILED",
          !g_big_file ? "not requested" : !ok64 ? "not reached" : okbig ? "OK" : "FAILED");
cleanup:
    WindowsDeleteString(cls); WindowsDeleteString(perm); WindowsDeleteString(star); WindowsDeleteString(path); WindowsDeleteString(token);
    if (list) list->lpVtbl->base.Release(list);
    if (permissions) permissions->lpVtbl->base.Release(permissions);
    if (item) __x_ABI_CWindows_CStorage_CIStorageItem_Release(item);
    if (folder) __x_ABI_CWindows_CStorage_CIStorageFolder_Release(folder);
    if (info) IAsyncInfo_Release(info);
    if (op) __FIAsyncOperation_1_Windows__CStorage__CStorageFolder_Release(op);
    if (filter) __FIVector_1_HSTRING_Release(filter);
    if (picker) picker->lpVtbl->base.Release(picker);
    if (instance) IInspectable_Release(instance);
}
