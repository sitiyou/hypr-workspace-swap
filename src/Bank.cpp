#include "Bank.hpp"
#include "Hyprland057.hpp"

#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/ipc/s2/S2.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/workspace/State.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/workspace/query/Query.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <algorithm>

using namespace WorkspaceSwap;

int CBank::currentWorkspace() const {
    const auto monitor = Desktop::focusState()->monitor();
    if (!monitor || !monitor->enabled() || monitor->m_activeSpecialWorkspace || !monitor->m_activeWorkspace)
        return 0;
    const auto id = monitor->m_activeWorkspace->numberedID();
    return id && *id >= 1 && *id <= 10 ? static_cast<int>(*id) : 0;
}

int CBank::activeBank() const {
    const auto id = currentWorkspace();
    return id ? m_states[id - 1].active : 0;
}

std::string& CBank::storage(size_t index, int bank) {
    auto& state = m_states[index];
    auto [entry, inserted] = state.storage.try_emplace(bank);
    if (inserted) {
        const auto slot = state.nextSlot++;
        entry->second = slot == 2 ? std::format("special:{}", index + 1) : std::format("special:workspace-swap-{}-{}", index + 1, slot);
    }
    return entry->second;
}

SDispatchResult CBank::cycle(int direction) {
    const auto bank = activeBank();
    if (!bank)
        return {.success = false, .error = {}};
    const auto next = direction > 0 ? (bank == m_count ? 1 : bank + 1) : (bank == 1 ? m_count : bank - 1);
    return select(next);
}

SDispatchResult CBank::select(int bank, bool all) {
    if (m_busy)
        return {.success = false, .error = "hypr-workspace-swap is already exchanging workspaces"};
    if (bank < 1 || bank > m_count)
        return {.success = false, .error = "bank is outside the configured range"};
    std::array<bool, 10> selected{};
    if (all)
        selected.fill(true);
    else {
        const auto id = currentWorkspace();
        if (!id)
            return {.success = false, .error = {}};
        selected[id - 1] = true;
    }
    m_busy = true;
    const Hyprutils::Utils::CScopeGuard guard([this] { m_busy = false; });
    return selectWorkspaces(bank, selected);
}

SDispatchResult CBank::selectWorkspaces(int bank, const std::array<bool, 10>& selected, bool validate) {
    WorkspacePairs pairs{};
    std::array<std::string, 10> addresses;
    for (size_t index = 0; index < pairs.size(); ++index) {
        if (!selected[index] || m_states[index].active == bank)
            continue;
        addresses[index] = storage(index, bank);
        pairs[index].normal = State::Workspace::state()->query().numbered(Workspace::SWorkspaceNumberedID{static_cast<uint32_t>(index + 1)}).run();
        pairs[index].special = State::Workspace::state()->query().input(addresses[index]).run();
    }
    if (std::ranges::all_of(addresses, [](const auto& address) { return address.empty(); }))
        return {};
    if (validate) {
        if (const auto result = Hyprland057::validate(pairs); !result.success)
            return result;
    }
    for (size_t index = 0; index < pairs.size(); ++index) {
        auto& pair = pairs[index];
        if ((!pair.normal || pair.normal->getWindowCount() == 0) && (!pair.special || pair.special->getWindowCount() == 0)) {
            pair = {};
            continue;
        }
        if (!pair.normal)
            pair.normal = State::Workspace::state()->createNumbered(Workspace::SWorkspaceNumberedID{static_cast<uint32_t>(index + 1)}, pair.special->m_monitor.lock(), {}, false);
        if (pair.normal && !pair.special)
            pair.special = State::Workspace::state()->createSpecial(addresses[index], pair.normal->m_monitor.lock(), false);
        if (!pair.normal || !pair.special)
            return {.success = false, .error = "hypr-workspace-swap could not create a workspace pair"};
    }
    if (validate) {
        if (const auto result = Hyprland057::validate(pairs); !result.success)
            return result;
    }
    for (size_t index = 0; index < pairs.size(); ++index) {
        if (addresses[index].empty())
            continue;
        auto& state = m_states[index];
        state.storage[state.active] = addresses[index];
        state.storage.erase(bank);
        state.active = bank;
    }
    Hyprland057::exchange(pairs);
    for (const auto& pair : pairs) {
        if (!pair.normal)
            continue;
        pair.normal->ready();
        pair.special->ready();
    }
    for (size_t index = 0; index < pairs.size(); ++index) {
        if (!addresses[index].empty())
            IPC::Socket2::sock()->postEvent({.event = "workspace_swap", .data = std::format("{},{}", index + 1, bank)});
    }
    return {};
}

SDispatchResult CBank::configure(int count) {
    if (m_busy)
        return {.success = false, .error = "hypr-workspace-swap is already exchanging workspaces"};
    if (count < 1)
        return {.success = false, .error = "bank_count must be a positive integer"};
    if (count >= m_count) {
        m_count = count;
        return {};
    }
    m_busy = true;
    const Hyprutils::Utils::CScopeGuard guard([this] { m_busy = false; });
    std::array<bool, 10> selected{};
    for (size_t index = 0; index < selected.size(); ++index)
        selected[index] = m_states[index].active > count;
    if (const auto result = selectWorkspaces(1, selected, false); !result.success)
        return result;
    for (size_t index = 0; index < m_states.size(); ++index) {
        auto& state = m_states[index];
        for (auto entry = state.storage.upper_bound(count); entry != state.storage.end();) {
            const auto source = State::Workspace::state()->query().input(entry->second).run();
            if (source && source->getWindowCount() > 0) {
                auto destination = state.active == 1
                    ? State::Workspace::state()->query().numbered(Workspace::SWorkspaceNumberedID{static_cast<uint32_t>(index + 1)}).run()
                    : State::Workspace::state()->query().input(storage(index, 1)).run();
                if (!destination) {
                    destination = state.active == 1
                        ? State::Workspace::state()->createNumbered(Workspace::SWorkspaceNumberedID{static_cast<uint32_t>(index + 1)}, source->m_monitor.lock(), {}, false)
                        : State::Workspace::state()->createSpecial(storage(index, 1), source->m_monitor.lock(), false);
                }
                if (!destination)
                    return {.success = false, .error = "could not create bank 1 workspace"};
                const auto windows = Desktop::windowState()->windows();
                for (const auto& window : windows) {
                    if (window->mapped() && window->m_workspace == source)
                        Desktop::globalWindowController()->moveWindowToWorkspace(window, destination);
                }
                destination->ready();
            }
            entry = state.storage.erase(entry);
        }
    }
    m_count = count;
    return {};
}
