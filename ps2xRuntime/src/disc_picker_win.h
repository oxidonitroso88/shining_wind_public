#ifndef PS2X_DISC_PICKER_WIN_H
#define PS2X_DISC_PICKER_WIN_H

// Windows folder dialog used by the double-click launch flow (see launch_options.h, chooseDiscInteractively).

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include <filesystem>
#include <optional>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace ps2x::launch
{
    inline std::wstring widen(const std::string &utf8)
    {
        if (utf8.empty())
            return {};
        const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(std::max(count, 1)) - 1u, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), count);
        return wide;
    }

    // Returns the chosen disc image file or extracted-disc folder, or nullopt when the user cancels.
    // `reason` (if any) is shown first in a message box.
    inline std::optional<std::filesystem::path> pickDisc(const std::string &reason)
    {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::optional<std::filesystem::path> result;
        if (!reason.empty())
            MessageBoxW(nullptr, widen(reason).c_str(), L"Shining Wind", MB_OK | MB_ICONINFORMATION);

        const int mode = MessageBoxW(nullptr,
                                     L"Select your Shining Wind (SLPM-66671) game.\n\n"
                                     L"Yes  = choose the disc image file (.iso)\n"
                                     L"No   = choose the folder where you extracted the disc\n"
                                     L"Cancel = exit",
                                     L"Shining Wind", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (mode == IDCANCEL)
        {
            if (SUCCEEDED(init))
                CoUninitialize();
            return result;
        }
        const bool pickFolder = (mode == IDNO);

        IFileOpenDialog *dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            DWORD options = 0;
            dialog->GetOptions(&options);
            if (pickFolder)
            {
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                dialog->SetTitle(L"Select the folder with your extracted Shining Wind disc (it contains SLPM_666.71)");
            }
            else
            {
                dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
                dialog->SetTitle(L"Select your Shining Wind disc image");
                const COMDLG_FILTERSPEC filters[] = {{L"Disc images (*.iso;*.bin;*.img;*.mdf)", L"*.iso;*.bin;*.img;*.mdf"},
                                                     {L"All files", L"*.*"}};
                dialog->SetFileTypes(2, filters);
            }
            if (SUCCEEDED(dialog->Show(nullptr)))
            {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    {
                        result = std::filesystem::path(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        return result;
    }

    // True for a build without a console window started by double click: nobody would ever see a message printed to stderr.
    // (A script that redirects stderr to a file or pipe keeps getting plain text and never blocks on a dialog.)
    inline bool noUsableErrorOutput()
    {
        const HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
        return handle == nullptr || handle == INVALID_HANDLE_VALUE || GetFileType(handle) == FILE_TYPE_UNKNOWN;
    }

    inline void showLaunchError(const std::string &message)
    {
        MessageBoxW(nullptr, widen(message).c_str(), L"Shining Wind", MB_OK | MB_ICONERROR);
    }
}

#endif
#endif
