#include "Bank.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

static_assert(std::string_view{GIT_COMMIT_HASH} == "0aa6745f705f2c4b0a5004eb019ef76e5bbdb426", "hypr-workspace-swap requires Hyprland v0.57.0 headers");

static std::unique_ptr<WorkspaceSwap::CBank> bank;

static int result(lua_State* state, const SDispatchResult& result) {
    lua_pushboolean(state, result.success);
    if (!result.success) {
        lua_pushlstring(state, result.error.data(), result.error.size());
        return 2;
    }
    return 1;
}

static int integer(lua_State* state, int index) {
    const auto value = luaL_checkinteger(state, index);
    luaL_argcheck(state, value >= 1 && value <= std::numeric_limits<int>::max(), index, "expected a positive integer in range");
    return static_cast<int>(value);
}

static int next(lua_State* state) {
    return result(state, bank->cycle(1));
}

static int prev(lua_State* state) {
    return result(state, bank->cycle(-1));
}

static int selectBank(lua_State* state) {
    return result(state, bank->select(integer(state, 1)));
}

static int selectAll(lua_State* state) {
    return result(state, bank->select(integer(state, 1), true));
}

static int configure(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "bank_count");
    return result(state, bank->configure(integer(state, -1)));
}

static int activeBank(lua_State* state) {
    const auto active = bank->activeBank();
    if (active)
        lua_pushinteger(state, active);
    else
        lua_pushnil(state);
    return 1;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    if (std::string_view{__hyprland_api_get_hash()} != __hyprland_api_get_client_hash())
        throw std::runtime_error("hypr-workspace-swap: compositor and plugin headers do not match");

    bank = std::make_unique<WorkspaceSwap::CBank>();
    if (!HyprlandAPI::addLuaFunction(handle, "workspace_swap", "toggle", next) ||
        !HyprlandAPI::addLuaFunction(handle, "workspace_swap", "next", next) ||
        !HyprlandAPI::addLuaFunction(handle, "workspace_swap", "prev", prev) ||
        !HyprlandAPI::addLuaFunction(handle, "workspace_swap", "select", selectBank) ||
        !HyprlandAPI::addLuaFunction(handle, "workspace_swap", "select_all", selectAll) ||
        !HyprlandAPI::addLuaFunction(handle, "workspace_swap", "configure", configure) ||
        !HyprlandAPI::addLuaFunction(handle, "workspace_swap", "bank", activeBank))
        throw std::runtime_error("hypr-workspace-swap: could not register plugin functions");

    return {"hypr-workspace-swap", "Independent workspace banks with local and global selection without rebuilding layouts", "hypr-workspace-swap contributors", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    bank.reset();
}
