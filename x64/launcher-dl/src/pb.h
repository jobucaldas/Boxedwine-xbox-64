// SPDX-License-Identifier: GPL-2.0-only
// Minimal protobuf wire-format writer/reader: enough for the handful of Steam
// messages steamdl needs, without generated code or a protobuf runtime.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

struct PbW {
    std::string b;
    void varint(uint64_t v) {
        while (v >= 0x80) { b += (char)(v | 0x80); v >>= 7; }
        b += (char)v;
    }
    void tag(int f, int wt) { varint((uint64_t)f << 3 | wt); }
    PbW& u(int f, uint64_t v) { tag(f, 0); varint(v); return *this; }
    PbW& s(int f, const std::string& v) { tag(f, 2); varint(v.size()); b += v; return *this; }
    PbW& f64(int f, uint64_t v) {
        tag(f, 1);
        for (int i = 0; i < 8; i++) b += (char)(v >> (8 * i));
        return *this;
    }
};

struct PbField {
    int num = 0;
    int wt = 0;
    uint64_t v = 0;   // varint / fixed value
    std::string s;    // length-delimited payload
};

struct Pb {
    std::vector<PbField> f;
    bool ok = false;

    Pb() {}
    explicit Pb(const std::string& in) { ok = parse(in); }

    bool parse(const std::string& in) {
        size_t i = 0, n = in.size();
        const unsigned char* p = (const unsigned char*)in.data();
        auto rv = [&](uint64_t& out) {
            out = 0;
            for (int sh = 0; sh < 64; sh += 7) {
                if (i >= n) return false;
                unsigned char c = p[i++];
                out |= (uint64_t)(c & 0x7f) << sh;
                if (!(c & 0x80)) return true;
            }
            return false;
        };
        while (i < n) {
            uint64_t key;
            if (!rv(key)) return false;
            PbField fl;
            fl.num = (int)(key >> 3);
            fl.wt = (int)(key & 7);
            if (fl.wt == 0) {
                if (!rv(fl.v)) return false;
            } else if (fl.wt == 1 || fl.wt == 5) {
                int w = fl.wt == 1 ? 8 : 4;
                if (i + w > n) return false;
                for (int k = 0; k < w; k++) fl.v |= (uint64_t)p[i + k] << (8 * k);
                i += w;
            } else if (fl.wt == 2) {
                uint64_t len;
                if (!rv(len) || len > n - i) return false;
                fl.s.assign(in, i, (size_t)len);
                i += (size_t)len;
            } else {
                return false;
            }
            f.push_back(std::move(fl));
        }
        return true;
    }
    const PbField* get(int num) const {
        for (auto& x : f) if (x.num == num) return &x;
        return nullptr;
    }
    uint64_t u(int num, uint64_t def = 0) const { auto* x = get(num); return x ? x->v : def; }
    std::string s(int num) const { auto* x = get(num); return x ? x->s : std::string(); }
    bool has(int num) const { return get(num) != nullptr; }
    std::vector<const PbField*> all(int num) const {
        std::vector<const PbField*> r;
        for (auto& x : f) if (x.num == num) r.push_back(&x);
        return r;
    }
};
