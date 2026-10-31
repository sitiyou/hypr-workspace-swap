#include "Bank.hpp"
#include "Hyprland057.hpp"

#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/workspace/State.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/workspace/query/Query.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace WorkspaceSwap;

int CBank::activeBank() const {
    return m_activeBank;
}

SDispatchResult CBank::toggle() {
    if (m_busy)
        return {.success = false, .error = "workspace-swap is already exchanging workspaces"};

    m_busy = true;
    const Hyprutils::Utils::CScopeGuard guard([this] { m_busy = false; });

    const auto monitor = Desktop::focusState()->monitor();
    if (!monitor || !monitor->enabled())
        return {.success = false, .error = "workspace-swap needs an active monitor"};

    WorkspacePairs pairs;
    for (size_t index = 0; index < pairs.size(); ++index) {
        const auto id        = static_cast<uint32_t>(index + 1);
        pairs[index].normal  = State::Workspace::state()->query().numbered(Workspace::SWorkspaceNumberedID{id}).run();
        pairs[index].special = State::Workspace::state()->query().input(std::format("special:{}", id)).run();
    }

    if (const auto result = Hyprland057::validate(pairs); !result.success)
        return result;

    for (size_t index = 0; index < pairs.size(); ++index) {
        auto&      pair = pairs[index];
        const auto id   = static_cast<uint32_t>(index + 1);
        if (!pair.normal) {
            auto targetMonitor = Config::workspaceRuleMgr()->getBoundMonitorForWS(std::to_string(id));
            if (!targetMonitor)
                targetMonitor = monitor;
            pair.normal = State::Workspace::state()->createNumbered(Workspace::SWorkspaceNumberedID{id}, targetMonitor);
        }
        if (pair.normal && !pair.special)
            pair.special = State::Workspace::state()->createSpecial(std::to_string(id), pair.normal->m_monitor.lock());
        if (!pair.normal || !pair.special)
            return {.success = false, .error = "workspace-swap could not create a workspace pair"};
    }

    m_pairs = pairs;
    for (const auto& pair : pairs) {
        pair.normal->ready();
        pair.special->ready();
    }

    if (const auto result = Hyprland057::validate(pairs); !result.success)
        return result;

    m_activeBank = 3 - m_activeBank;
    Hyprland057::exchange(pairs);
    IPC::Socket2::sock()->postEvent({.event = "workspace_swap", .data = std::to_string(m_activeBank)});
    return {};
}
