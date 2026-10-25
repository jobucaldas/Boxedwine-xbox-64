// SPDX-License-Identifier: GPL-2.0-only
#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

std::string hexLower(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string h;
    h.reserve(raw.size() * 2);
    for (unsigned char c : raw) { h += d[c >> 4]; h += d[c & 15]; }
    return h;
}

namespace {
struct Sha1Ctx {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    bool ok = false;
    Sha1Ctx() {
        ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) == 0 &&
             BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0;
    }
    ~Sha1Ctx() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void update(const void* p, size_t n) {
        if (ok && n) ok = BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0) == 0;
    }
    std::string finish() {
        std::string out(20, '\0');
        if (!ok || BCryptFinishHash(h, (PUCHAR)&out[0], 20, 0) != 0) return std::string();
        return out;
    }
};

bool aes(const std::string& key, const wchar_t* mode, std::string iv, const std::string& in, bool pad, std::string& out) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE k = NULL;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0) return false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)mode, (ULONG)((wcslen(mode) + 1) * sizeof(wchar_t)), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) == 0) {
        ULONG flags = pad ? BCRYPT_BLOCK_PADDING : 0;
        ULONG n = 0;
        out.assign(in.size() + 16, '\0');
        if (BCryptDecrypt(k, (PUCHAR)in.data(), (ULONG)in.size(), NULL, iv.empty() ? NULL : (PUCHAR)&iv[0], (ULONG)iv.size(),
                          (PUCHAR)&out[0], (ULONG)out.size(), &n, flags) == 0) {
            out.resize(n);
            ok = true;
        }
    }
    if (k) BCryptDestroyKey(k);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
}  // namespace

std::string sha1(const std::string& data) {
    Sha1Ctx c;
    c.update(data.data(), data.size());
    return c.finish();
}

std::string sha1File(const std::wstring& path, bool& ok) {
    ok = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return std::string();
    Sha1Ctx c;
    std::vector<char> buf(1 << 20);
    DWORD got = 0;
    bool rd = true;
    while ((rd = ReadFile(f, buf.data(), (DWORD)buf.size(), &got, NULL)) && got) c.update(buf.data(), got);
    CloseHandle(f);
    if (!rd) return std::string();
    std::string h = c.finish();
    ok = h.size() == 20;
    return h;
}

bool steamSymmetricDecrypt(const std::string& key, const std::string& in, std::string& out) {
    if (key.size() != 32 || in.size() < 32 || (in.size() % 16) != 0) return false;
    std::string iv;
    if (!aes(key, BCRYPT_CHAIN_MODE_ECB, std::string(), in.substr(0, 16), false, iv) || iv.size() != 16) return false;
    return aes(key, BCRYPT_CHAIN_MODE_CBC, iv, in.substr(16), true, out);
}
