// Headless install: mirrors the wizard validation order (game version and
// region, title-update compatibility, TU8-for-DLC, space check) and then
// runs the same Installer::install with console progress.

#include "installer_cli.h"

#include <chrono>
#include <cstdio>

#include <install/installer.h>
#include <install/xbox360/title_update_manager.h>
#include <user/config.h>

namespace
{

bool Fail(const std::string& message)
{
    std::fprintf(stderr, "install: error: %s\n", message.c_str());
    fflush(stderr);
    return false;
}

} // namespace

int InstallerCli::RunHeadlessInstall(const std::filesystem::path& installPath,
                                     const std::string& isoSource,
                                     const std::string& tuSource,
                                     const std::vector<std::string>& dlcSources)
{
    if (isoSource.empty())
        return (Fail("no --install-iso given"), 1);

    std::filesystem::path gameSourcePath(isoSource);
    if (!Installer::parseGame(gameSourcePath))
        return (Fail("not a valid GTA IV source: " + isoSource), 1);

    GameVersionResult version = Installer::validateGameVersion(gameSourcePath);
    if (!version.isValid)
        return (Fail(version.errorTitle + ": " + version.errorMessage), 1);
    if (!version.isCorrectGame || !version.isCorrectRegion)
        return (Fail("wrong game or region: " + version.errorMessage), 1);
    std::printf("install: game source OK: %s\n", isoSource.c_str());

    liberty::install::TitleUpdateManager titleUpdateManager;
    if (!tuSource.empty())
    {
        std::filesystem::path tuPath(tuSource);
        std::error_code ec;
        if (std::filesystem::is_directory(tuPath, ec))
        {
            int found = titleUpdateManager.ScanDirectory(tuPath);
            std::printf("install: scanned %s, %d update(s)\n", tuSource.c_str(), found);
        }
        else
        {
            if (!titleUpdateManager.AddTitleUpdate(tuPath))
                return (Fail("not a valid title update: " + tuSource), 1);
        }
        const auto& updates = titleUpdateManager.GetDetectedUpdates();
        if (updates.empty())
            return (Fail("no title updates found in: " + tuSource), 1);
        // Newest compatible update wins; log the choice.
        int best = -1;
        for (size_t i = 0; i < updates.size(); i++)
        {
            std::string compat = Installer::checkGameUpdateCompatibility(gameSourcePath, updates[i].path);
            if (compat.empty() && (best < 0 || updates[i].info.version > updates[static_cast<size_t>(best)].info.version))
                best = static_cast<int>(i);
        }
        if (best < 0)
            return (Fail("no compatible title update in: " + tuSource), 1);
        titleUpdateManager.SelectUpdate(best);
        std::printf("install: update: %s\n",
                    liberty::install::TitleUpdateManager::GetUpdateDisplayName(updates[static_cast<size_t>(best)].info).c_str());
    }

    std::array<std::filesystem::path, 2> dlcPaths;
    for (const auto& dlcSource : dlcSources)
    {
        std::filesystem::path path(dlcSource);
        DLC dlc = Installer::parseDLC(path);
        if (dlc == DLC::Unknown)
            return (Fail("not a valid DLC package: " + dlcSource), 1);
        dlcPaths[static_cast<size_t>(dlc) - 1] = path;
        std::printf("install: DLC: %s\n", dlcSource.c_str());
    }

    bool anyDlc = !dlcPaths[0].empty() || !dlcPaths[1].empty();
    if (anyDlc)
    {
        const auto& updates = titleUpdateManager.GetDetectedUpdates();
        bool tu8 = false;
        if (titleUpdateManager.HasSelectedUpdate())
        {
            if (auto selected = titleUpdateManager.GetSelectedUpdate())
                tu8 = selected->info.version >= 8;
        }
        if (!tu8)
            return (Fail("DLC requires Title Update 1.06 (TU8); pass --install-tu with a TU8 container"), 1);
    }

    Installer::Input input;
    input.gameSource = gameSourcePath;
    if (titleUpdateManager.HasSelectedUpdate())
    {
        if (auto selected = titleUpdateManager.GetSelectedUpdate())
            input.updateSource = selected->path;
    }
    for (const auto& path : dlcPaths)
    {
        if (!path.empty())
            input.dlcSources.push_back(path);
    }

    Installer::Sources sources;
    Journal journal;
    if (!Installer::parseSources(input, journal, sources))
        return (Fail("invalid sources: " + journal.lastErrorMessage), 1);

    std::error_code spaceError;
    std::uint64_t available = std::filesystem::space(installPath, spaceError).available;
    if (!spaceError && sources.totalSize > available)
    {
        char message[256];
        std::snprintf(message, sizeof(message), "not enough disk space: need %.2f GiB, have %.2f GiB",
                      double(sources.totalSize) / 1073741824.0, double(available) / 1073741824.0);
        return (Fail(message), 1);
    }

    std::printf("install: installing %.2f GiB to %s\n",
                double(sources.totalSize) / 1073741824.0, installPath.string().c_str());
    fflush(stdout);
    if (!Installer::install(sources, installPath, false, journal, std::chrono::seconds(1), [&] {
            int percent = journal.progressTotal == 0
                ? 0
                : static_cast<int>(100 * journal.progressCounter / journal.progressTotal);
            std::printf("\rinstall: %d%% (%llu/%llu)", percent,
                        static_cast<unsigned long long>(journal.progressCounter),
                        static_cast<unsigned long long>(journal.progressTotal));
            fflush(stdout);
            return true;
        }))
    {
        std::printf("\n");
        Installer::rollback(journal);
        return (Fail(journal.lastErrorMessage), 2);
    }
    std::printf("\ninstall: done\n");

    if (!dlcPaths[0].empty())
        Config::DLCSourceDir = dlcPaths[0].parent_path().string();
    else if (!dlcPaths[1].empty())
        Config::DLCSourceDir = dlcPaths[1].parent_path().string();
    if (titleUpdateManager.HasSelectedUpdate())
    {
        if (auto selected = titleUpdateManager.GetSelectedUpdate())
            Config::UpdateSourceDir = selected->path.parent_path().string();
    }
    Config::Save();
    return 0;
}
