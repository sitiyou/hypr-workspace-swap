#include <sstream>
#include <hyprland/src/plugins/PluginAPI.hpp>

#define private public
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#undef private

#include "Hyprland057.hpp"

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/workspace/WorkspaceRuleManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/layout/supplementary/WorkspaceAlgoMatcher.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <vector>

using namespace WorkspaceSwap;

struct SWindowMove {
    PHLWINDOW    window;
    PHLWORKSPACE destination;
};

struct SFloatingMove {
    SP<Layout::CSpace>  space;
    SP<Layout::ITarget> target;
    CBox               box;
};

static PHLWORKSPACE destinationFor(const WorkspacePairs& pairs, const PHLWORKSPACE& workspace) {
    for (const auto& pair : pairs) {
        if (workspace == pair.normal)
            return pair.special;
        if (workspace == pair.special)
            return pair.normal;
    }
    return nullptr;
}

static std::string layoutFor(const PHLWORKSPACE& workspace) {
    return Layout::Supplementary::algoMatcher()->getNameForTiledAlgo(workspace->space()->algorithm()->tiledAlgo().get());
}

SDispatchResult WorkspaceSwap::Hyprland057::validate(const WorkspacePairs& pairs) {
    if (g_layoutManager->dragController()->target())
        return {.success = false, .error = "workspace-swap cannot exchange workspaces during a window drag"};

    for (const auto& pair : pairs) {
        if (pair.special && pair.special->visible())
            return {.success = false, .error = std::format("close {} before exchanging workspaces", pair.special->addressableName())};

        for (const auto& workspace : {pair.normal, pair.special}) {
            if (!workspace)
                continue;
            if (!workspace->m_monitor || !workspace->m_monitor->enabled() || !workspace->space() || !workspace->space()->algorithm())
                return {.success = false, .error = "workspace-swap found a workspace without an enabled monitor or layout"};
            const auto layout = layoutFor(workspace);
            if (layout != "master" && layout != "dwindle")
                return {.success = false, .error = "workspace-swap supports only native master and dwindle layouts"};
            static auto defaultLayout = CConfigValue<Config::STRING>("general:layout");
            const auto rule = Config::workspaceRuleMgr()->getWorkspaceRuleFor(workspace).value_or(Config::CWorkspaceRule{});
            if (layout != rule.m_layout.value_or(*defaultLayout))
                return {.success = false, .error = "workspace-swap requires the current layout to match its workspace rule"};
        }

        if (pair.normal && pair.special) {
            if (layoutFor(pair.normal) != layoutFor(pair.special))
                return {.success = false, .error = "workspace-swap requires matching layouts on each normal/special pair"};
            const auto normalRule  = Config::workspaceRuleMgr()->getWorkspaceRuleFor(pair.normal).value_or(Config::CWorkspaceRule{});
            const auto specialRule = Config::workspaceRuleMgr()->getWorkspaceRuleFor(pair.special).value_or(Config::CWorkspaceRule{});
            if (normalRule.m_layoutopts != specialRule.m_layoutopts)
                return {.success = false, .error = "workspace-swap requires matching layout options on each normal/special pair"};
        }
    }

    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->mapped() || !window->m_workspace)
            continue;
        const bool affected = std::ranges::any_of(pairs, [&](const auto& pair) { return window->m_workspace == pair.normal || window->m_workspace == pair.special; });
        if (affected && (window->m_state & Desktop::View::WINDOW_STATE_PINNED))
            return {.success = false, .error = "unpin affected windows before exchanging workspaces"};
    }
    return {};
}

void WorkspaceSwap::Hyprland057::exchange(const WorkspacePairs& pairs) {
    std::vector<SWindowMove>   windows;
    std::vector<SFloatingMove> floats;
    std::vector<PHLWORKSPACE>  movedSpecials;
    std::array<std::pair<PHLWINDOW, PHLWINDOW>, 10> focus;

    const auto focusedMonitor = Desktop::focusState()->monitor();
    const auto focusWorkspace = focusedMonitor && !focusedMonitor->m_activeSpecialWorkspace ? focusedMonitor->m_activeWorkspace : nullptr;
    const bool restoreFocus   = focusWorkspace && destinationFor(pairs, focusWorkspace);

    for (const auto& window : Desktop::windowState()->windows()) {
        if (!window->mapped() || !window->m_workspace)
            continue;
        if (const auto destination = destinationFor(pairs, window->m_workspace))
            windows.push_back({window, destination});
    }

    for (size_t index = 0; index < pairs.size(); ++index) {
        const auto& pair = pairs[index];
        if (!pair.normal)
            continue;
        focus[index]     = {pair.normal->getLastFocusedWindow(), pair.special->getLastFocusedWindow()};
        const auto delta = pair.normal->m_monitor->m_position - pair.special->m_monitor->m_position;
        if (pair.normal->m_monitor == pair.special->m_monitor)
            continue;
        movedSpecials.push_back(pair.special);
        for (const auto& target : pair.special->space()->targets()) {
            if (target && target->floating())
                floats.push_back({pair.special->space(), target.lock(), target->position().translate(delta)});
        }
    }

    for (const auto& pair : pairs) {
        if (!pair.normal)
            continue;
        pair.special->m_monitor = pair.normal->m_monitor;
        std::swap(pair.normal->m_space, pair.special->m_space);
        pair.normal->m_space->m_parent  = pair.normal;
        pair.special->m_space->m_parent = pair.special;
    }

    // Synchronous event callbacks must see the entire batch's final membership.
    for (const auto& move : windows) {
        move.window->m_workspace = move.destination;
        move.window->m_monitor   = move.destination->m_monitor;
    }

    for (size_t index = 0; index < pairs.size(); ++index) {
        const auto& pair = pairs[index];
        if (!pair.normal)
            continue;
        pair.normal->rememberFocusedWindow(focus[index].second);
        pair.special->rememberFocusedWindow(focus[index].first);
        pair.normal->space()->recheckWorkArea();
        pair.special->space()->recheckWorkArea();
    }

    for (const auto& move : floats)
        move.space->algorithm()->setTargetGeom(move.box, move.target);

    for (const auto& move : windows) {
        const auto& window = move.window;
        window->syncInitialWorkspaceToken(move.destination);
        window->updateFullscreenInputState();
        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN)->setValueAndWarp(window->isBlockedByFullscreen() ? 0.F : 1.F);
        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->setCallbackOnEnd(nullptr);
        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_MOVE_TO_WORKSPACE)->setValueAndWarp(1.F);
        window->presentation().alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->setValueAndWarp(1.F);
        window->presentation().resetMonitorMovedFrom();
        window->presentation().setAnimationsToMove();
        window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_ON_WORKSPACE);
        window->updateWindowData();
        window->updateToplevel();
        window->presentation().uncacheDecorations();
        window->presentation().updateDecorations();
    }

    for (const auto& pair : pairs) {
        if (!pair.normal)
            continue;
        pair.normal->space()->recalculate(Layout::RECALCULATE_REASON_WORKSPACE_CHANGE);
        pair.special->space()->recalculate(Layout::RECALCULATE_REASON_WORKSPACE_CHANGE);
        g_pHyprRenderer->damageMonitor(pair.normal->m_monitor.lock());
    }

    Desktop::globalWindowController()->updateSuspendedStates();
    if (restoreFocus)
        Desktop::focusState()->fullWindowFocus(focusWorkspace->getFocusCandidate(), Desktop::FOCUS_REASON_KEYBIND);
    g_pInputManager->recheckIdleInhibitorStatus();

    for (const auto& workspace : movedSpecials) {
        const auto monitor = workspace->m_monitor.lock();
        workspace->m_events.monitorChanged.emit();
        IPC::Socket2::sock()->postEvent({.event = "moveworkspace", .data = std::format("{},{}", workspace->addressableName(), monitor->m_name)});
        IPC::Socket2::sock()->postEvent(
            {.event = "moveworkspacev2", .data = std::format("{},{},{}", workspace->addressableName(), workspace->displayName(), monitor->m_name)});
        Event::bus()->m_events.workspace.moveToMonitor.emit(workspace, monitor);
    }

    for (const auto& move : windows) {
        if (!move.window->mapped() || move.window->m_workspace != move.destination)
            continue;
        IPC::Socket2::sock()->postEvent({.event = "movewindow", .data = std::format("{:x},{}", reinterpret_cast<uintptr_t>(move.window.get()), move.destination->addressableName())});
        IPC::Socket2::sock()->postEvent({.event = "movewindowv2",
                                       .data  = std::format("{:x},{},{}", reinterpret_cast<uintptr_t>(move.window.get()), move.destination->addressableName(),
                                                            move.destination->displayName())});
        Event::bus()->m_events.window.moveToWorkspace.emit(move.window, move.destination);
    }
}
