#include "Bank.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lua.h>
}

#include <memory>
#include <stdexcept>
#include <string_view>

static_assert(std::string_view{GIT_COMMIT_HASH} == "0aa6745f705f2c4b0a5004eb019ef76e5bbdb426", "workspace-swap requires Hyprland v0.57.0 headers");

static std::unique_ptr<WorkspaceSwap::CBank> bank;

static int toggle(lua_State* state) {
    const auto result = bank->toggle();
    lua_pushboolean(state, result.success);
    if (!result.success) {
        lua_pushlstring(state, result.error.data(), result.error.size());
        return 2;
    }
    return 1;
}

static int activeBank(lua_State* state) {
    lua_pushinteger(state, bank->activeBank());
    return 1;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (std::string_view{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash())
        throw std::runtime_error("workspace-swap: compositor and plugin headers do not match");

    bank = std::make_unique<WorkspaceSwap::CBank>();
    if (!HyprlandAPI::addLuaFunction(handle, "workspace_swap", "toggle", toggle) ||
        !HyprlandAPI::addLuaFunction(handle, "workspace_swap", "bank", activeBank))
        throw std::runtime_error("workspace-swap: could not register plugin functions");

    return {"workspace-swap", "Exchange workspaces 1-10 with special:1-special:10 without rebuilding layouts", "workspace-swap contributors", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    bank.reset();
}
