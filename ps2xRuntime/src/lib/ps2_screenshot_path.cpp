// F12 screenshots: where to save them. Kept in its own file because <windows.h>/<shlobj.h> clash with raylib's names in ps2_runtime.cpp.
// The folder is "<Pictures>\ShiningWind" (the user's real Pictures folder, also when it is redirected to OneDrive), falling back to %USERPROFILE%\Pictures and
// finally to the working directory. The file is ShiningWind_YYYYMMDD-HHMMSS.png (a -2, -3... suffix if two shots fall in the same second).
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#endif

std::string ps2x_nextScreenshotPath()
{
    namespace fs = std::filesystem;
    fs::path base;
#if defined(_WIN32)
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &known)) && known)
    {
        base = fs::path(known);
        CoTaskMemFree(known);
    }
    else if (const char *profile = std::getenv("USERPROFILE"))
    {
        base = fs::path(profile) / "Pictures";
    }
#else
    if (const char *home = std::getenv("HOME"))
        base = fs::path(home) / "Pictures";
#endif
    fs::path dir = base.empty() ? fs::path(".") : base / "ShiningWind";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        dir = ".";

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    fs::path path = dir / (std::string("ShiningWind_") + stamp + ".png");
    for (int n = 2; fs::exists(path, ec) && n < 1000; ++n)
        path = dir / (std::string("ShiningWind_") + stamp + "-" + std::to_string(n) + ".png");
    return path.string();
}
