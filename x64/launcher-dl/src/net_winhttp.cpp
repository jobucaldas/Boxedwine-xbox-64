// SPDX-License-Identifier: GPL-2.0-only
// WinHTTP transport (desktop Windows and stock Wine). TLS is the host's
// (schannel on Windows, Wine's schannel->GnuTLS off-device).
#include "net.h"

#include <windows.h>
#include <winhttp.h>

#include <deque>

static std::wstring widen(const std::string& s) {
    std::wstring w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string lastErr(const char* what) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s failed, error %lu", what, (unsigned long)GetLastError());
    return buf;
}

static HINTERNET session() {
    static HINTERNET s = NULL;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending = FALSE;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (pending) {
        s = WinHttpOpen(L"steamdl/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (s)
            WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
        InitOnceComplete(&once, 0, NULL);
    }
    return s;
}

class WinHttpGet : public Http {
public:
    bool get(const std::string& host, const std::string& path, HttpResult& out) override {
        out = HttpResult();
        HINTERNET ses = session();
        if (!ses) { out.err = lastErr("WinHttpOpen"); return false; }
        HINTERNET con = WinHttpConnect(ses, widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!con) { out.err = lastErr("WinHttpConnect"); return false; }
        bool ok = false;
        HINTERNET req = WinHttpOpenRequest(con, L"GET", widen(path).c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (req) {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        }
        if (!req) {
            out.err = lastErr("WinHttpOpenRequest");
        } else if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            out.err = lastErr("WinHttpSendRequest");
        } else if (!WinHttpReceiveResponse(req, NULL)) {
            out.err = lastErr("WinHttpReceiveResponse");
        } else {
            DWORD status = 0, len = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            out.status = (int)status;
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail)) { out.err = lastErr("WinHttpQueryDataAvailable"); ok = false; break; }
                if (!avail) break;
                if (out.body.size() + avail > 128 * 1024 * 1024) { out.err = "HTTP body exceeds limit"; ok = false; break; }
                size_t at = out.body.size();
                out.body.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, &out.body[at], avail, &got)) { out.err = lastErr("WinHttpReadData"); ok = false; break; }
                out.body.resize(at + got);
            }
        }
        if (req) WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        return ok;
    }
};

// Receiving runs on its own thread so recv() can time out (for heartbeats)
// while WinHttpWebSocketReceive blocks.
class WinHttpWebSocket : public WebSocket {
public:
    WinHttpWebSocket() {
        InitializeCriticalSection(&lock_);
        InitializeConditionVariable(&cv_);
    }
    ~WinHttpWebSocket() override {
        close();
        DeleteCriticalSection(&lock_);
    }
    bool connect(const std::string& host, int port, const std::string& path, std::string& err) override {
        HINTERNET ses = session();
        if (!ses) { err = lastErr("WinHttpOpen"); return false; }
        con_ = WinHttpConnect(ses, widen(host).c_str(), (INTERNET_PORT)port, 0);
        if (!con_) { err = lastErr("WinHttpConnect"); return false; }
        HINTERNET req = WinHttpOpenRequest(con_, L"GET", widen(path).c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) { err = lastErr("WinHttpOpenRequest"); return false; }
        bool ok = false;
        if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
            err = lastErr("WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET");
        } else if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            err = lastErr("WinHttpSendRequest");
        } else if (!WinHttpReceiveResponse(req, NULL)) {
            err = lastErr("WinHttpReceiveResponse");
        } else if (!(ws_ = WinHttpWebSocketCompleteUpgrade(req, 0))) {
            err = lastErr("WinHttpWebSocketCompleteUpgrade");
        } else {
            ok = true;
        }
        WinHttpCloseHandle(req);
        if (!ok) return false;
        thread_ = CreateThread(NULL, 0, &WinHttpWebSocket::rxThread, this, 0, NULL);
        return thread_ != NULL;
    }
    bool send(const std::string& msg) override {
        if (!ws_) return false;
        return WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID)msg.data(), (DWORD)msg.size()) == NO_ERROR;
    }
    int recv(std::string& msg, int timeoutMs) override {
        EnterCriticalSection(&lock_);
        DWORD start = GetTickCount();
        while (q_.empty() && !closed_) {
            DWORD el = GetTickCount() - start;
            if ((int)el >= timeoutMs) break;
            SleepConditionVariableCS(&cv_, &lock_, timeoutMs - el);
        }
        int r;
        if (!q_.empty()) { msg.swap(q_.front()); q_.pop_front(); r = 1; }
        else r = closed_ ? -1 : 0;
        LeaveCriticalSection(&lock_);
        return r;
    }
    void close() override {
        if (ws_) {
            WinHttpWebSocketClose(ws_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        }
        if (thread_) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = NULL;
        }
        if (ws_) { WinHttpCloseHandle(ws_); ws_ = NULL; }
        if (con_) { WinHttpCloseHandle(con_); con_ = NULL; }
    }

private:
    static DWORD WINAPI rxThread(LPVOID p) {
        WinHttpWebSocket* self = (WinHttpWebSocket*)p;
        std::string cur;
        char buf[65536];
        for (;;) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            if (WinHttpWebSocketReceive(self->ws_, buf, sizeof(buf), &got, &type) != NO_ERROR) break;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            if (cur.size() + got > 128 * 1024 * 1024) break;
            cur.append(buf, got);
            if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                continue;
            EnterCriticalSection(&self->lock_);
            self->q_.push_back(std::string());
            self->q_.back().swap(cur);
            LeaveCriticalSection(&self->lock_);
            WakeAllConditionVariable(&self->cv_);
        }
        EnterCriticalSection(&self->lock_);
        self->closed_ = true;
        LeaveCriticalSection(&self->lock_);
        WakeAllConditionVariable(&self->cv_);
        return 0;
    }

    HINTERNET con_ = NULL;
    HINTERNET ws_ = NULL;
    HANDLE thread_ = NULL;
    CRITICAL_SECTION lock_;
    CONDITION_VARIABLE cv_;
    std::deque<std::string> q_;
    bool closed_ = false;
};

std::unique_ptr<Http> makeHttp() { return std::unique_ptr<Http>(new WinHttpGet()); }
std::unique_ptr<WebSocket> makeWebSocket() { return std::unique_ptr<WebSocket>(new WinHttpWebSocket()); }
