// Native desktop setup: ImGui-free front end for Installer.
// Mirrors InstallerWizard's pages (language, introduction, game, title
// update, DLC, space check, install, done/failed) with native SDL message
// boxes and NFD file dialogs. All validation and install logic is shared
// Installer core; only the prompting is native.

#include "installer_native.h"

#include <cstdio>
#include <thread>

#include <SDL3/SDL_messagebox.h>
#include <nfd.h>

#include <apu/embedded_player.h>
#include <install/installer.h>
#include <install/xbox360/title_update_manager.h>
#include <locale/locale.h>
#include <ui/game_window.h>
#include <user/config.h>

namespace
{

constexpr int kButtonCancel = -1;
constexpr int kButtonBack = -2;

SDL_Window* ParentWindow()
{
    return GameWindow::s_pWindow;
}

const char* AppCaption()
{
    return GameWindow::GetTitle();
}

// Native modal prompt. Returns the pressed button id, kButtonCancel when the
// dialog is dismissed without a choice.
int AskBox(SDL_MessageBoxFlags flags, const std::string& message,
           const std::vector<std::pair<const char*, int>>& buttons)
{
    std::vector<SDL_MessageBoxButtonData> nativeButtons;
    nativeButtons.reserve(buttons.size());
    for (const auto& [label, id] : buttons)
    {
        SDL_MessageBoxButtonData button = {};
        if (id == kButtonCancel)
            button.flags = SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        button.buttonID = id;
        button.text = label;
        nativeButtons.push_back(button);
    }

    SDL_MessageBoxData data = {};
    data.flags = flags;
    data.window = ParentWindow();
    data.title = AppCaption();
    data.message = message.c_str();
    data.numbuttons = static_cast<int>(nativeButtons.size());
    data.buttons = nativeButtons.data();

    int pressedId = kButtonCancel;
    if (SDL_ShowMessageBox(&data, &pressedId) != 0)
        return kButtonCancel;
    return pressedId;
}

bool AskQuit()
{
    return AskBox(SDL_MESSAGEBOX_WARNING, Localise("Installer_Message_Quit"),
                  {{"Quit", 1}, {"Back", 0}}) == 1;
}

bool PickFiles(std::list<std::filesystem::path>& outPaths)
{
    const nfdpathset_t* pathSet = nullptr;
    nfdresult_t result = NFD_OpenDialogMultipleN(&pathSet, nullptr, 0, nullptr);
    if (result == NFD_OKAY)
    {
        nfdpathsetsize_t count = 0;
        bool ok = NFD_PathSet_GetCount(pathSet, &count) == NFD_OKAY;
        for (nfdpathsetsize_t i = 0; ok && i < count; i++)
        {
            nfdnchar_t* itemPath = nullptr;
            if (NFD_PathSet_GetPathN(pathSet, i, &itemPath) != NFD_OKAY)
            {
                ok = false;
                break;
            }
            std::printf("[Picker] picked: %s\n", itemPath);
            outPaths.emplace_back(std::filesystem::path(itemPath));
            NFD_PathSet_FreePathN(itemPath);
        }
        fflush(stdout);
        NFD_PathSet_Free(pathSet);
        if (!ok)
        {
            outPaths.clear();
            AskBox(SDL_MESSAGEBOX_ERROR, "Could not read the selected paths from the file dialog.",
                   {{"OK", 0}});
            return false;
        }
        return true;
    }
    if (result == NFD_ERROR)
    {
        AskBox(SDL_MESSAGEBOX_ERROR, NFD_GetError(), {{"OK", 0}});
        return false;
    }
    return false; // Cancelled.
}

bool PickFolder(std::filesystem::path& outPath)
{
    nfdnchar_t* folderPath = nullptr;
    nfdresult_t result = NFD_PickFolderN(&folderPath, nullptr);
    if (result == NFD_OKAY)
    {
        std::printf("[Picker] picked folder: %s\n", folderPath);
        fflush(stdout);
        outPath = std::filesystem::path(folderPath);
        NFD_PathSet_FreePathN(folderPath);
        return true;
    }
    if (result == NFD_ERROR)
        AskBox(SDL_MESSAGEBOX_ERROR, NFD_GetError(), {{"OK", 0}});
    return false; // Cancelled.
}

const char* kLanguageLabels[] = {"FRANCAIS", "DEUTSCH", "ENGLISH", "ESPANOL", "ITALIANO", "Japanese"};
const ELanguage kLanguageValues[] = {
    ELanguage::French, ELanguage::German, ELanguage::English,
    ELanguage::Spanish, ELanguage::Italian, ELanguage::Japanese,
};

// Mirrors the wizard: language -> introduction -> game -> title update ->
// DLC -> space check -> install -> done. Each step loops on invalid input;
// Cancel asks for quit confirmation and aborts with false.
bool RunSteps(const std::filesystem::path& installPath, bool skipGame,
              liberty::install::TitleUpdateManager& titleUpdateManager)
{
    // Language + introduction (skipped in DLC-only mode, like the wizard).
    if (!skipGame)
    {
        for (;;)
        {
            std::vector<std::pair<const char*, int>> langButtons;
            for (int i = 0; i < 6; i++)
                langButtons.emplace_back(kLanguageLabels[i], i);
            langButtons.emplace_back("Cancel", kButtonCancel);
            int langChoice = AskBox(SDL_MESSAGEBOX_INFORMATION,
                                    Localise("Installer_Page_SelectLanguage"), langButtons);
            if (langChoice < 0 || langChoice > 5)
            {
                if (AskQuit())
                    return false;
                continue; // Back: re-offer the language choice.
            }
            Config::Language = kLanguageValues[langChoice];
            break;
        }

        if (AskBox(SDL_MESSAGEBOX_INFORMATION, Localise("Installer_Page_Introduction"),
                   {{"Next", 0}, {"Cancel", kButtonCancel}}) != 0)
            return false;
    }

    // Game source.
    std::filesystem::path gameSourcePath;
    if (!skipGame)
    {
        for (;;)
        {
            int choice = AskBox(SDL_MESSAGEBOX_INFORMATION, Localise("Installer_Page_SelectGame"),
                                {{"Add ISO / files", 0}, {"Add folder", 1}, {"Cancel", kButtonCancel}});
            if (choice == kButtonCancel)
                return AskQuit() ? false : RunSteps(installPath, skipGame, titleUpdateManager);
            std::list<std::filesystem::path> picked;
            bool havePaths = false;
            if (choice == 0)
            {
                havePaths = PickFiles(picked);
            }
            else
            {
                std::filesystem::path folder;
                if (PickFolder(folder))
                {
                    picked.push_back(folder);
                    havePaths = true;
                }
                else
                {
                    continue; // Cancelled folder pick: re-offer.
                }
            }
            if (!havePaths)
                continue; // Cancelled file pick: re-offer.

            std::string failedNames;
            for (const auto& path : picked)
            {
                if (Installer::parseGame(path))
                    gameSourcePath = path;
                else
                    failedNames += "\n- " + path.filename().string();
            }
            if (!failedNames.empty())
            {
                AskBox(SDL_MESSAGEBOX_ERROR,
                       Localise("Installer_Message_InvalidFilesList") + failedNames,
                       {{"OK", 0}});
            }
            if (!gameSourcePath.empty())
                break;
        }

        // Same version/region validation as the wizard's game page.
        for (;;)
        {
            GameVersionResult version = Installer::validateGameVersion(gameSourcePath);
            if (version.isValid && version.isCorrectGame && version.isCorrectRegion)
                break;
            std::string error = !version.isValid
                ? version.errorTitle + "\n\n" + version.errorMessage
                : Localise("Installer_Message_WrongGame") + "\n\n" + version.errorMessage;
            if (AskBox(SDL_MESSAGEBOX_ERROR, error, {{"Pick again", 0}, {"Quit", 1}}) == 1)
                return false;
            gameSourcePath.clear();
            // Re-pick: fall through to outer game loop by restarting steps.
            return RunSteps(installPath, skipGame, titleUpdateManager);
        }
    }

    // Title update: scan the same folders as the wizard, offer folder browse.
    int selectedUpdateIndex = -1;
    if (!skipGame)
    {
        titleUpdateManager.Clear();
        std::filesystem::path updatesDir = std::filesystem::current_path() / "GAME UPDATES";
        if (std::filesystem::exists(updatesDir))
            titleUpdateManager.ScanDirectory(updatesDir);
        std::filesystem::path installUpdatesDir = installPath / "updates";
        if (std::filesystem::exists(installUpdatesDir))
            titleUpdateManager.ScanDirectory(installUpdatesDir);

        for (;;)
        {
            const auto& updates = titleUpdateManager.GetDetectedUpdates();
            std::vector<std::pair<const char*, int>> buttons;
            std::vector<std::string> labels;
            for (size_t i = 0; i < updates.size() && i < 10; i++)
            {
                labels.push_back(liberty::install::TitleUpdateManager::GetUpdateDisplayName(updates[i].info));
                buttons.emplace_back(labels.back().c_str(), static_cast<int>(i));
            }
            buttons.emplace_back("No update", -10);
            buttons.emplace_back("Browse folder", -11);
            buttons.emplace_back("Cancel", kButtonCancel);
            int choice = AskBox(SDL_MESSAGEBOX_INFORMATION,
                                Localise("Installer_Page_SelectTitleUpdate"), buttons);
            if (choice == kButtonCancel)
                return AskQuit() ? false : RunSteps(installPath, skipGame, titleUpdateManager);
            if (choice == -11)
            {
                std::filesystem::path folder;
                if (PickFolder(folder))
                    titleUpdateManager.ScanDirectory(folder);
                continue;
            }
            if (choice >= 0)
            {
                std::string compatError = Installer::checkGameUpdateCompatibility(
                    gameSourcePath, updates[static_cast<size_t>(choice)].path);
                if (!compatError.empty())
                {
                    AskBox(SDL_MESSAGEBOX_ERROR, "Incompatible Title Update:\n\n" + compatError,
                           {{"OK", 0}});
                    continue;
                }
                selectedUpdateIndex = choice;
                titleUpdateManager.SelectUpdate(choice);
            }
            else
            {
                titleUpdateManager.SelectUpdate(-1);
            }
            break;
        }
    }

    // DLC / episodes.
    std::array<std::filesystem::path, 2> dlcSourcePaths;
    for (;;)
    {
        std::string prompt = Localise("Installer_Page_SelectDLC");
        if (!dlcSourcePaths[0].empty())
            prompt += std::string("\n- TLAD: ") + dlcSourcePaths[0].filename().string();
        if (!dlcSourcePaths[1].empty())
            prompt += std::string("\n- TBOGT: ") + dlcSourcePaths[1].filename().string();
        int choice = AskBox(SDL_MESSAGEBOX_INFORMATION, prompt,
                            {{"Add files", 0}, {"Add folder", 1}, {"Done", 2}, {"Cancel", kButtonCancel}});
        if (choice == kButtonCancel)
            return AskQuit() ? false : RunSteps(installPath, skipGame, titleUpdateManager);
        if (choice == 2)
            break;
        std::list<std::filesystem::path> picked;
        bool havePaths = false;
        if (choice == 0)
        {
            havePaths = PickFiles(picked);
        }
        else
        {
            std::filesystem::path folder;
            if (PickFolder(folder))
            {
                picked.push_back(folder);
                havePaths = true;
            }
            else
            {
                continue;
            }
        }
        if (!havePaths)
            continue;
        std::string failedNames;
        for (const auto& path : picked)
        {
            DLC dlc = Installer::parseDLC(path);
            if (dlc != DLC::Unknown)
                dlcSourcePaths[static_cast<size_t>(dlc) - 1] = path;
            else
                failedNames += "\n- " + path.filename().string();
        }
        if (!failedNames.empty())
        {
            AskBox(SDL_MESSAGEBOX_ERROR,
                   Localise("Installer_Message_InvalidFilesList") + failedNames, {{"OK", 0}});
        }
    }

    // Episodes need TU8+. Mirror the wizard's requirement check.
    bool anyDlc = !dlcSourcePaths[0].empty() || !dlcSourcePaths[1].empty();
    if (anyDlc)
    {
        const auto& updates = titleUpdateManager.GetDetectedUpdates();
        bool tu8ok = selectedUpdateIndex >= 0 &&
            selectedUpdateIndex < static_cast<int>(updates.size()) &&
            updates[static_cast<size_t>(selectedUpdateIndex)].info.version >= 8;
        if (!tu8ok)
        {
            bool tu8available = false;
            for (const auto& update : updates)
            {
                if (update.info.version == 8)
                {
                    tu8available = true;
                    break;
                }
            }
            std::string error = "DLC Content Requires Title Update 1.06 (TU8)\n\n"
                "The Lost and Damned and The Ballad of Gay Tony\n"
                "require the latest title update to function correctly.";
            std::vector<std::pair<const char*, int>> buttons;
            if (tu8available)
                buttons.emplace_back("Use TU8", 0);
            buttons.emplace_back("Change files", 1);
            buttons.emplace_back("Quit", 2);
            int fix = AskBox(SDL_MESSAGEBOX_ERROR, error, buttons);
            if (fix == 2)
                return false;
            if (fix == 0 && tu8available)
            {
                for (size_t i = 0; i < updates.size(); i++)
                {
                    if (updates[i].info.version == 8)
                    {
                        selectedUpdateIndex = static_cast<int>(i);
                        titleUpdateManager.SelectUpdate(static_cast<int>(i));
                        break;
                    }
                }
            }
            else
            {
                // Re-pick: restart steps so sources can change.
                return RunSteps(installPath, skipGame, titleUpdateManager);
            }
        }
    }

    // Parse sources + space check (mirrors the wizard's CheckSpace page).
    Installer::Input installerInput;
    installerInput.gameSource = gameSourcePath;
    if (titleUpdateManager.HasSelectedUpdate())
    {
        if (auto selected = titleUpdateManager.GetSelectedUpdate())
            installerInput.updateSource = selected->path;
    }
    for (const auto& path : dlcSourcePaths)
    {
        if (!path.empty())
            installerInput.dlcSources.push_back(path);
    }

    Installer::Sources sources;
    Journal journal;
    std::string sourcesError;
    if (!Installer::parseSources(installerInput, journal, sources))
    {
        sourcesError = journal.lastErrorMessage;
        if (AskBox(SDL_MESSAGEBOX_ERROR,
                   Localise("Installer_Message_InvalidFiles") + "\n\n" + sourcesError,
                   {{"Pick again", 0}, {"Quit", 1}}) == 1)
            return false;
        return RunSteps(installPath, skipGame, titleUpdateManager);
    }

    std::error_code spaceError;
    std::uintmax_t available = std::filesystem::space(installPath, spaceError).available;
    if (!spaceError && sources.totalSize > static_cast<std::uint64_t>(available))
    {
        constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
        char message[512];
        std::snprintf(message, sizeof(message), "Not enough disk space.\nRequired: %.2f GiB\nAvailable: %.2f GiB",
                      double(sources.totalSize) / GiB, double(static_cast<std::uint64_t>(available)) / GiB);
        if (AskBox(SDL_MESSAGEBOX_ERROR, message, {{"Pick again", 0}, {"Quit", 1}}) == 1)
            return false;
        return RunSteps(installPath, skipGame, titleUpdateManager);
    }

    {
        constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
        char message[512];
        std::snprintf(message, sizeof(message), "Ready to install.\nRequired: %.2f GiB\nAvailable: %.2f GiB",
                      double(sources.totalSize) / GiB,
                      spaceError ? 0.0 : double(static_cast<std::uint64_t>(available)) / GiB);
        if (AskBox(SDL_MESSAGEBOX_INFORMATION, message, {{"Install", 0}, {"Cancel", kButtonCancel}}) != 0)
            return AskQuit() ? false : RunSteps(installPath, skipGame, titleUpdateManager);
    }

    // Install on a worker thread with console progress, mirroring the wizard.
    std::atomic<bool> failed = false;
    std::atomic<int> lastPercent = -1;
    std::string installError;
    std::thread worker([&] {
        if (!Installer::install(sources, installPath, false, journal, std::chrono::seconds(1), [&] {
                int percent = journal.progressTotal == 0
                    ? 0
                    : static_cast<int>(100 * journal.progressCounter / journal.progressTotal);
                int previous = lastPercent.exchange(percent);
                if (percent != previous)
                {
                    std::printf("\r[Install] %d%% (%llu/%llu)", percent,
                                static_cast<unsigned long long>(journal.progressCounter),
                                static_cast<unsigned long long>(journal.progressTotal));
                    fflush(stdout);
                }
                return true;
            }))
        {
            failed = true;
            installError = journal.lastErrorMessage;
            Installer::rollback(journal);
        }
    });
    worker.join();
    std::printf("\n");
    fflush(stdout);

    if (failed)
    {
        if (AskBox(SDL_MESSAGEBOX_ERROR, "Installation failed:\n\n" + installError,
                   {{"Retry", 0}, {"Quit", 1}}) == 1)
            return false;
        return RunSteps(installPath, skipGame, titleUpdateManager);
    }

    // Persist source dirs for the auto-installer, like the wizard.
    if (!dlcSourcePaths[0].empty())
        Config::DLCSourceDir = dlcSourcePaths[0].parent_path().string();
    else if (!dlcSourcePaths[1].empty())
        Config::DLCSourceDir = dlcSourcePaths[1].parent_path().string();
    if (titleUpdateManager.HasSelectedUpdate())
    {
        if (auto selected = titleUpdateManager.GetSelectedUpdate())
            Config::UpdateSourceDir = selected->path.parent_path().string();
    }
    Config::Save();

    AskBox(SDL_MESSAGEBOX_INFORMATION, Localise("Installer_Page_InstallSucceeded"), {{"Play", 0}});
    return true;
}

} // namespace

bool InstallerNative::Run(const std::filesystem::path& installPath, bool skipGame)
{
    // Native setup must be windowed so dialogs and the console stay usable.
    bool wasFullscreen = GameWindow::IsFullscreen();
    if (wasFullscreen)
        GameWindow::SetFullscreen(false);

    EmbeddedPlayer::Init();
    NFD_Init();

    liberty::install::TitleUpdateManager titleUpdateManager;
    bool installed = RunSteps(installPath, skipGame, titleUpdateManager);

    NFD_Quit();
    EmbeddedPlayer::Shutdown();

    if (wasFullscreen)
        GameWindow::SetFullscreen(true);

    return installed;
}
