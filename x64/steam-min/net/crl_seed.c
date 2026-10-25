/* B70 O2: pre-provision genuine CRLs into a Wine prefix's URL cache (cryptnet's own cache).
 * Usage: crl_seed.exe <crl-url> <crl.der> <issuer.der> [...triples]
 * Valve's steamclient verifies TLS with CertVerifyRevocation; Wine's cryptnet then fetches
 * the CA's http:// CRL with no timeout, and on this box TCP :80 is blackholed, so the call
 * hangs past Valve's 20 s request timeout ("status = 0"). The same CRL is fetched by the
 * caller over HTTPS from the CA's own host; here it is checked (issuer name, issuer
 * signature, not expired) and committed under its http:// URL with expiry = NextUpdate,
 * exactly as cryptnet caches a wire fetch. Revocation checking stays fully on: a revoked
 * serial is still found in the CRL. No TLS or certificate exceptions are added. */
#include <windows.h>
#include <wincrypt.h>
#include <wininet.h>
#include <stdio.h>

static BYTE *slurp(const wchar_t *path, DWORD *len) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(f, NULL); BYTE *b = HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (!b || !ReadFile(f, b, n, len, NULL) || *len != n) { CloseHandle(f); return NULL; }
    CloseHandle(f); return b;
}

static int seed(const wchar_t *url, const wchar_t *crl_path, const wchar_t *issuer_path) {
    DWORD crl_len = 0, iss_len = 0, n = 0;
    BYTE *crl_der = slurp(crl_path, &crl_len), *iss_der = slurp(issuer_path, &iss_len);
    if (!crl_der || !iss_der) { printf("crl_seed=0 url=%ls reason=read\n", url); return 0; }
    PCCRL_CONTEXT crl = CertCreateCRLContext(X509_ASN_ENCODING, crl_der, crl_len);
    PCCERT_CONTEXT iss = CertCreateCertificateContext(X509_ASN_ENCODING, iss_der, iss_len);
    if (!crl || !iss) { printf("crl_seed=0 url=%ls reason=parse err=0x%08lx\n", url, GetLastError()); return 0; }
    if (!CertCompareCertificateName(X509_ASN_ENCODING, &crl->pCrlInfo->Issuer, &iss->pCertInfo->Subject)) {
        printf("crl_seed=0 url=%ls reason=issuer_name\n", url); return 0;
    }
    if (!CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_CRL, (void *)crl,
                                           CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT, (void *)iss, 0, NULL)) {
        printf("crl_seed=0 url=%ls reason=signature err=0x%08lx\n", url, GetLastError()); return 0;
    }
    FILETIME now; GetSystemTimeAsFileTime(&now);
    FILETIME next = crl->pCrlInfo->NextUpdate;
    if (CompareFileTime(&next, &now) <= 0) { printf("crl_seed=0 url=%ls reason=expired\n", url); return 0; }

    wchar_t file[MAX_PATH];
    if (!CreateUrlCacheEntryW(url, crl_len, L"crl", file, 0)) {
        printf("crl_seed=0 url=%ls reason=cache_create err=%lu\n", url, GetLastError()); return 0;
    }
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, crl_der, crl_len, &n, NULL) || n != crl_len) {
        printf("crl_seed=0 url=%ls reason=cache_write err=%lu\n", url, GetLastError());
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return 0;
    }
    CloseHandle(f);
    FILETIME zero = {0};
    if (!CommitUrlCacheEntryW(url, file, next, zero, NORMAL_CACHE_ENTRY, NULL, 0, NULL, NULL)) {
        printf("crl_seed=0 url=%ls reason=cache_commit err=%lu\n", url, GetLastError()); return 0;
    }
    /* Read it back the way cryptnet will: cache only, no wire. */
    PCCRL_CONTEXT back = NULL;
    BOOL hit = CryptRetrieveObjectByUrlW(url, CONTEXT_OID_CRL, CRYPT_CACHE_ONLY_RETRIEVAL, 0,
                                         (void **)&back, NULL, NULL, NULL, NULL);
    SYSTEMTIME st; FileTimeToSystemTime(&next, &st);
    printf("crl_seed=%d url=%ls entries=%lu next_update=%04u-%02u-%02uT%02u:%02uZ cache_hit=%d\n", hit ? 1 : 0, url,
           crl->pCrlInfo->cCRLEntry, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, hit ? 1 : 0);
    if (back) CertFreeCRLContext(back);
    CertFreeCRLContext(crl); CertFreeCertificateContext(iss);
    return hit ? 1 : 0;
}

int wmain(int argc, wchar_t **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4 || (argc - 1) % 3) { fprintf(stderr, "usage: crl_seed.exe URL CRL.der ISSUER.der [...]\n"); return 2; }
    int ok = 0, total = 0;
    for (int i = 1; i + 2 < argc; i += 3, total++) ok += seed(argv[i], argv[i + 1], argv[i + 2]);
    printf("crl_seed_total=%d ok=%d\n", total, ok);
    return ok == total ? 0 : 1;
}
