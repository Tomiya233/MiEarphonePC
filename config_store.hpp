#pragma once
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace miearphone {

struct AppConfig {
    std::string last_mac = "00:BB:43:7B:CD:0B";
    bool close_to_tray = true;
    bool auto_connect = true;
    bool auto_reconnect = true;
    bool dark_mode = false;

    static std::wstring config_path() {
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::wstring p(exe_path);
        auto slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            p = p.substr(0, slash + 1);
        }
        return p + L"MiEarphonePC.ini";
    }

    void load() {
        std::wstring path = config_path();
        FILE* fp = _wfopen(path.c_str(), L"rb");
        if (!fp) return;
        char line[512];
        while (std::fgets(line, sizeof(line), fp)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            auto eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq);
            std::string v = s.substr(eq + 1);
            if (k == "last_mac" && !v.empty()) last_mac = v;
            else if (k == "close_to_tray") close_to_tray = (v == "1" || v == "true");
            else if (k == "auto_connect") auto_connect = (v == "1" || v == "true");
            else if (k == "auto_reconnect") auto_reconnect = (v == "1" || v == "true");
            else if (k == "dark_mode") dark_mode = (v == "1" || v == "true");
        }
        std::fclose(fp);
    }

    void save() const {
        std::wstring path = config_path();
        FILE* fp = _wfopen(path.c_str(), L"wb");
        if (!fp) return;
        std::fprintf(fp, "last_mac=%s\r\n", last_mac.c_str());
        std::fprintf(fp, "close_to_tray=%d\r\n", close_to_tray ? 1 : 0);
        std::fprintf(fp, "auto_connect=%d\r\n", auto_connect ? 1 : 0);
        std::fprintf(fp, "auto_reconnect=%d\r\n", auto_reconnect ? 1 : 0);
        std::fprintf(fp, "dark_mode=%d\r\n", dark_mode ? 1 : 0);
        std::fclose(fp);
    }

    static constexpr const wchar_t* kRunRegPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static constexpr const wchar_t* kRunValName = L"MiEarphonePC";

    static bool is_autostart_enabled() {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
            return false;
        }
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD cb = sizeof(buf);
        DWORD type = 0;
        LSTATUS st = RegQueryValueExW(hKey, kRunValName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
        RegCloseKey(hKey);
        return (st == ERROR_SUCCESS && type == REG_SZ && buf[0] != L'\0');
    }

    static bool set_autostart_enabled(bool enable) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
            return false;
        }
        LSTATUS st = ERROR_SUCCESS;
        if (enable) {
            wchar_t exe_path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(exe_path) + L"\" --tray";
            st = RegSetValueExW(hKey, kRunValName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(cmd.c_str()),
                                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        } else {
            st = RegDeleteValueW(hKey, kRunValName);
            if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
        }
        RegCloseKey(hKey);
        return st == ERROR_SUCCESS;
    }
};

} // namespace miearphone
