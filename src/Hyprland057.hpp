#pragma once

#include "Bank.hpp"

namespace WorkspaceSwap::Hyprland057 {
    SDispatchResult validate(const WorkspacePairs& pairs);
    void            exchange(const WorkspacePairs& pairs);
}
