// SPDX-License-Identifier: GPL-2.0-only
// Text KeyValues (VDF) parser for PICS appinfo buffers.
#pragma once
#include <string>
#include <vector>

struct Kv {
    std::string key, val;
    std::vector<Kv> kids;
    bool obj = false;

    const Kv* child(const std::string& k) const {
        for (auto& c : kids)
            if (eq(c.key, k)) return &c;
        return nullptr;
    }
    std::string str(const std::string& k) const {
        const Kv* c = child(k);
        return c && !c->obj ? c->val : std::string();
    }
    static bool eq(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
        return true;
    }
};

class VdfParser {
public:
    explicit VdfParser(const std::string& t) : t_(t) {}
    // Parses `"key" { ... }` at top level into root.
    bool parse(Kv& root) {
        std::string k;
        if (!tok(k) || k == "{" || k == "}") return false;
        root.key = k;
        return value(root);
    }

private:
    bool value(Kv& n) {
        std::string v;
        size_t save = i_;
        bool q = false;
        if (!tok(v, &q)) return false;
        if (v == "{" && !q) {
            n.obj = true;
            for (;;) {
                std::string k;
                bool kq = false;
                if (!tok(k, &kq)) return false;
                if (k == "}" && !kq) return true;
                Kv c;
                c.key = k;
                if (!value(c)) return false;
                n.kids.push_back(std::move(c));
            }
        }
        (void)save;
        n.val = v;
        return true;
    }
    bool tok(std::string& out, bool* quoted = nullptr) {
        out.clear();
        for (;;) {
            while (i_ < t_.size() && (isspace((unsigned char)t_[i_]) || t_[i_] == '\0')) i_++;
            if (i_ + 1 < t_.size() && t_[i_] == '/' && t_[i_ + 1] == '/') {
                while (i_ < t_.size() && t_[i_] != '\n') i_++;
                continue;
            }
            break;
        }
        if (i_ >= t_.size()) return false;
        if (quoted) *quoted = false;
        char c = t_[i_];
        if (c == '{' || c == '}') { out = c; i_++; return true; }
        if (c == '"') {
            if (quoted) *quoted = true;
            i_++;
            while (i_ < t_.size() && t_[i_] != '"') {
                if (t_[i_] == '\\' && i_ + 1 < t_.size()) {
                    char e = t_[++i_];
                    out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else {
                    out += t_[i_];
                }
                i_++;
            }
            if (i_ >= t_.size()) return false;
            i_++;
            return true;
        }
        while (i_ < t_.size() && !isspace((unsigned char)t_[i_]) && t_[i_] != '{' && t_[i_] != '}' && t_[i_] != '"') out += t_[i_++];
        return true;
    }
    const std::string& t_;
    size_t i_ = 0;
};
