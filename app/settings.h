// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app settings: a plain "key=value" file in ~/Library/Application Support/KAVL/settings.txt, read at start and written when the app quits.
#pragma once
#include <map>
#include <string>

class Settings {
public:
    void load();
    void save() const;
    std::string str(const std::string& k, const std::string& def = "") const;
    double num(const std::string& k, double def) const;
    bool flag(const std::string& k, bool def) const { return num(k, def ? 1 : 0) != 0; }
    void set(const std::string& k, const std::string& v) { kv_[k] = v; }
    void set(const std::string& k, double v);
    void set(const std::string& k, bool v) { set(k, v ? 1.0 : 0.0); }
    void set(const std::string& k, int v) { set(k, static_cast<double>(v)); }
    static std::string dir();   // ~/Library/Application Support/KAVL
private:
    std::map<std::string, std::string> kv_;
};
