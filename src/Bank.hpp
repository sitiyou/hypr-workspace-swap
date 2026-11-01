#pragma once

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <array>
#include <map>
#include <string>

namespace WorkspaceSwap {

    struct SWorkspacePair {
        PHLWORKSPACE normal;
        PHLWORKSPACE special;
    };

    using WorkspacePairs = std::array<SWorkspacePair, 10>;

    class CBank {
      public:
        SDispatchResult cycle(int direction);
        SDispatchResult select(int bank, bool all = false);
        SDispatchResult configure(int count);
        int             activeBank() const;

      private:
        struct SBankState {
            int active = 1;
            int nextSlot = 2;
            std::map<int, std::string> storage;
        };

        int currentWorkspace() const;
        std::string& storage(size_t index, int bank);
        SDispatchResult selectWorkspaces(int bank, const std::array<bool, 10>& selected, bool validate = true);

        std::array<SBankState, 10> m_states;
        int  m_count = 2;
        bool m_busy = false;
    };

}
