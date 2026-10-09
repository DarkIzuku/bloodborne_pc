// SPDX-License-Identifier: GPL-2.0-or-later
// Private settings for the native networking bridge. Renderer settings stay separate.
#pragma once
#include <atomic>
#include <cstdlib>
#include <string>

class BbNetSettingsImpl {
public:
    static BbNetSettingsImpl* GetInstance() { static BbNetSettingsImpl s; return &s; }
    static bool Flag(const char* name, bool fallback) {
        const char* v = std::getenv(name);
        return v ? v[0] == '1' : fallback;
    }
    static std::string Text(const char* name) {
        const char* v = std::getenv(name);
        return v ? v : "";
    }
    bool IsConnectedToNetwork() const { return Flag("BB_ONLINE", false); }
    bool IsShadNetEnabled() const { return IsConnectedToNetwork() && !shadnet_session_disabled.load(); }
    void SetShadNetSessionDisabled(bool v) { shadnet_session_disabled.store(v); }
    std::string GetShadNetServer() const { return Text("BB_SHADNET_SERVER"); }
    std::string GetShadNetWebApiServer() const { return Text("BB_SHADNET_WEBAPI"); }
    bool IsUPnPEnabled() const { return Flag("BB_UPNP", false); }
private:
    std::atomic<bool> shadnet_session_disabled{false};
};
#define EmulatorSettings (*BbNetSettingsImpl::GetInstance())
