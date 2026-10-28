#pragma once

#include <filesystem>

// Native desktop setup: same installer core as the ImGui wizard, driven by
// native message boxes and file dialogs in a windowed (never fullscreen) SDL
// window. No ImGui pages. Desktop only.
struct InstallerNative
{
    // Runs the full setup. Returns true when the game is installed and the
    // boot should continue, false when the user quit.
    static bool Run(const std::filesystem::path& installPath, bool skipGame);
};
