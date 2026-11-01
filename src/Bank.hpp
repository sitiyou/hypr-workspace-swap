#pragma once

#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>

#include <array>

namespace WorkspaceSwap {

    struct SWorkspacePair {
        PHLWORKSPACE normal;
        PHLWORKSPACE special;
    };

    using WorkspacePairs = std::array<SWorkspacePair, 10>;

    class CBank {
      public:
        SDispatchResult toggle();
        int             activeBank() const;

      private:
        int  m_activeBank = 1;
        bool m_busy       = false;
    };

}
