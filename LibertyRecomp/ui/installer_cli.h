#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Headless install: same Installer core as the wizards, no window, no
// dialogs. Returns a process exit code: 0 installed, 1 bad input,
// 2 install failed (rollback done).
struct InstallerCli
{
    static int RunHeadlessInstall(const std::filesystem::path& installPath,
                                  const std::string& isoSource,
                                  const std::string& tuSource,
                                  const std::vector<std::string>& dlcSources);
};
