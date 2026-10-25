// SPDX-License-Identifier: GPL-2.0-only
// SHA-1 and Steam's AES-256 "symmetric" scheme over BCrypt (bcrypt is on the
// UWP Win32 API list, so this file is shared by the desktop and console builds).
#pragma once
#include <string>

std::string sha1(const std::string& data);  // 20 raw bytes, empty on failure
std::string sha1File(const std::wstring& path, bool& ok);
// SteamKit2 CryptoHelper.SymmetricDecrypt: first 16 bytes are the IV encrypted
// with AES-256-ECB, the rest is AES-256-CBC with PKCS#7 padding.
bool steamSymmetricDecrypt(const std::string& key32, const std::string& in, std::string& out);
std::string hexLower(const std::string& raw);
