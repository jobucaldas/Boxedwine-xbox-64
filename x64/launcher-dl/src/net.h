// SPDX-License-Identifier: GPL-2.0-only
// Transport seam for steamdl. Everything above this interface (CM protocol,
// PICS, manifests, chunk crypto/decompression, verification) is portable C++.
// net_winhttp.cpp implements it for desktop Windows / stock Wine.  WinHTTP is
// NOT on the UWP Win32 API list (DOCS-ANSWERS row 16), so the console build
// needs a second implementation over WinRT (Windows.Web.Http.HttpClient +
// Windows.Networking.Sockets.MessageWebSocket); see DECISION.md.
#pragma once
#include <memory>
#include <string>

struct HttpResult {
    int status = 0;
    std::string body;
    std::string err;
};

class Http {
public:
    virtual ~Http() {}
    // HTTPS GET https://host/path. Returns false on transport error.
    virtual bool get(const std::string& host, const std::string& path, HttpResult& out) = 0;
};

class WebSocket {
public:
    virtual ~WebSocket() {}
    virtual bool connect(const std::string& host, int port, const std::string& path, std::string& err) = 0;
    virtual bool send(const std::string& msg) = 0;
    // 1 = got a message, 0 = timeout, -1 = closed/error
    virtual int recv(std::string& msg, int timeoutMs) = 0;
    virtual void close() = 0;
};

std::unique_ptr<Http> makeHttp();
std::unique_ptr<WebSocket> makeWebSocket();
