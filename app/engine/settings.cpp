// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app settings (settings.h).
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

std::string Settings::dir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/KAVL";
}

void Settings::load() {
    std::ifstream f(dir() + "/settings.txt");
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
        kv_[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

void Settings::save() const {
    std::error_code ec;
    std::filesystem::create_directories(dir(), ec);
    std::ofstream f(dir() + "/settings.txt");
    f << "# KAVL settings\n";
    for (const auto& [k, v] : kv_) f << k << '=' << v << '\n';
}

std::string Settings::str(const std::string& k, const std::string& def) const {
    const auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
}

double Settings::num(const std::string& k, double def) const {
    const auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end && end != it->second.c_str() ? v : def;
}

void Settings::set(const std::string& k, double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.10g", v);
    kv_[k] = b;
}
