// See include/extras/features/soultree_process.h.
#include "extras/features/soultree_process.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace pc_extras::soultree_process
{
    bool environmentPassThrough(const std::string &name, const std::string &extra)
    {
        // Diagnostic dumps, traces, memory card / input paths ... never travel: the session can neither write elsewhere nor depend on a debugging switch.
        // Only the explicit test-only list does.
        size_t at = 0;
        while (at < extra.size())
        {
            size_t end = extra.find(',', at);
            if (end == std::string::npos)
                end = extra.size();
            if (extra.compare(at, end - at, name) == 0 && end - at == name.size())
                return true;
            at = end + 1;
        }
        return false;
    }

#if defined(_WIN32)
    namespace
    {
        std::wstring widen(const std::string &s)
        {
            if (s.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
            return w;
        }

        std::string narrow(const std::wstring &w)
        {
            if (w.empty())
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        struct WindowSearch
        {
            DWORD pid;
            bool found;
        };

        BOOL CALLBACK enumWindow(HWND hwnd, LPARAM lp)
        {
            auto *search = reinterpret_cast<WindowSearch *>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == search->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr)
            {
                RECT r{};
                if (GetWindowRect(hwnd, &r) && r.right - r.left > 100)
                {
                    search->found = true;
                    return FALSE;
                }
            }
            return TRUE;
        }

        class WinSession final : public Session
        {
        public:
            HANDLE process = nullptr, job = nullptr;
            DWORD pid = 0;
            std::filesystem::path dir;
            bool done = false;
            int code = -1;

            ~WinSession() override
            {
                terminate();
                if (process != nullptr)
                    CloseHandle(process);
                if (job != nullptr)
                    CloseHandle(job); // KILL_ON_JOB_CLOSE: nothing survives
                cleanup();
            }

            bool exited(int &exitCode) override
            {
                if (!done && process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
                {
                    DWORD c = 0;
                    GetExitCodeProcess(process, &c);
                    code = static_cast<int>(c);
                    done = true;
                }
                exitCode = code;
                return done;
            }

            bool windowVisible() override
            {
                if (done)
                    return false;
                WindowSearch s{pid, false};
                EnumWindows(enumWindow, reinterpret_cast<LPARAM>(&s));
                return s.found;
            }

            void terminate() override
            {
                int c = 0;
                if (!exited(c) && job != nullptr)
                {
                    TerminateJobObject(job, 43);
                    WaitForSingleObject(process, 3000);
                }
            }

            std::string logTail() override
            {
                std::ifstream f(dir / "child.log", std::ios::binary);
                if (!f)
                    return {};
                std::stringstream ss;
                ss << f.rdbuf();
                std::string all = ss.str();
                if (all.size() > 2000)
                    all = all.substr(all.size() - 2000);
                return all;
            }

            std::vector<std::string> diagnosticLines() override
            {
                std::vector<std::string> out;
                std::ifstream f(dir / "child.log", std::ios::binary);
                std::string line;
                static const char *const prefixes[] = {"[config]", "[gs]", "[story]", "[hdpack] pack", "[hdpack] no pack", "[soul]"};
                while (std::getline(f, line))
                {
                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                        line.pop_back();
                    for (const char *prefix : prefixes)
                        if (line.rfind(prefix, 0) == 0)
                        {
                            out.push_back(line.substr(0, 300));
                            break;
                        }
                }
                return out;
            }

            void cleanup()
            {
                // Only the private folder this session created (a "ps2x-soul-" folder directly under the temp directory) is ever removed.
                std::error_code ec;
                const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
                if (!ec && !dir.empty() && std::filesystem::equivalent(dir.parent_path(), temp, ec) && !ec && dir.filename().string().rfind("ps2x-soul-", 0) == 0)
                    std::filesystem::remove_all(dir, ec);
            }
        };
    }

    int removeStaleFolders()
    {
        std::error_code ec;
        const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
        if (ec)
            return 0;
        int removed = 0;
        for (std::filesystem::directory_iterator it(temp, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.rfind("ps2x-soul-", 0) != 0 || !it->is_directory(ec))
                continue;
            const unsigned long pid = std::strtoul(name.c_str() + 10, nullptr, 10);
            if (pid == 0ul || pid == GetCurrentProcessId())
                continue;
            bool alive = false;
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)))
            {
                DWORD code = 0;
                alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
                CloseHandle(h);
            }
            if (!alive)
            {
                std::error_code rc;
                std::filesystem::remove_all(it->path(), rc);
                if (!rc)
                    ++removed;
            }
        }
        return removed;
    }

    std::unique_ptr<Session> start(const std::string &exePath, const std::string &elfPath, const std::string &character, int door,
                                   const std::string &inputConfigPath, const std::string &extrasConfigPath,
                                   const std::vector<std::pair<std::string, std::string>> &settings, std::string &error)
    {
        std::error_code ec;
        if (exePath.empty() || !std::filesystem::exists(exePath, ec))
        {
            error = "the game executable was not found";
            return nullptr;
        }
        auto session = std::make_unique<WinSession>();
        const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);
        static unsigned counter = 0;
        session->dir = temp / ("ps2x-soul-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter));
        std::filesystem::create_directories(session->dir / "card", ec);
        if (ec)
        {
            error = "could not create the temporary folder";
            return nullptr;
        }
        // The player's key bindings are only read: the child gets a copy it may do what it wants with.
        const std::filesystem::path childInput = session->dir / "ps2x_input.cfg";
        if (!inputConfigPath.empty() && std::filesystem::exists(inputConfigPath, ec))
            std::filesystem::copy_file(inputConfigPath, childInput, ec);

        // environment: the parent's variables minus every PS2X_* that is not whitelisted, plus the session's own
        std::wstring env;
        const char *extraPass = std::getenv("PS2X_EXTRAS_CHILD_PASS");
        const std::string extra = extraPass != nullptr ? extraPass : "";
        if (LPWCH block = GetEnvironmentStringsW())
        {
            for (const wchar_t *p = block; *p != L'\0'; p += wcslen(p) + 1)
            {
                const std::wstring entry(p);
                const size_t eq = entry.find(L'=', 1);
                const std::string name = eq == std::wstring::npos ? std::string() : narrow(entry.substr(0, eq));
                if (name.rfind("PS2X_", 0) == 0 && !environmentPassThrough(name, extra))
                    continue;
                env += entry;
                env.push_back(L'\0');
            }
            FreeEnvironmentStringsW(block);
        }
        auto add = [&](const std::string &name, const std::string &value) {
            env += widen(name + "=" + value);
            env.push_back(L'\0');
        };
        if (const char *scripted = std::getenv("PS2X_EXTRAS_CHILD_TEST_INPUT"); scripted != nullptr && *scripted != '\0')
            add("PS2X_EXTRAS_TEST_INPUT", scripted); // test-only: a scripted input for the child's own Extras overlay
        for (const auto &setting : settings)
            add(setting.first, setting.second);
        if (const char *shot = std::getenv("PS2X_EXTRAS_CHILD_WINDOW_SHOT"); shot != nullptr && *shot != '\0')
        {
            // test-only: the session saves pictures of its own window with this prefix (the parent keeps its own PS2X_WINDOW_SHOT)
            add("PS2X_WINDOW_SHOT", shot);
            if (const char *every = std::getenv("PS2X_EXTRAS_CHILD_WINDOW_SHOT_EVERY"))
                add("PS2X_WINDOW_SHOT_EVERY", every);
        }
        add("PS2X_MC_ROOT", (session->dir / "card").string());
        add("PS2X_INPUT_CONFIG", childInput.string());
        if (!extrasConfigPath.empty())
            add("PS2X_EXTRAS_CONFIG", extrasConfigPath);
        env.push_back(L'\0');

        session->job = CreateJobObjectW(nullptr, nullptr);
        if (session->job == nullptr)
        {
            error = "could not create the job object";
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(session->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        const std::wstring logPath = widen((session->dir / "child.log").string());
        HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = log != INVALID_HANDLE_VALUE ? log : nul;
        si.hStdError = log != INVALID_HANDLE_VALUE ? log : nul;
        std::wstring cmd = L"\"" + widen(exePath) + L"\"";
        if (!elfPath.empty())
            cmd += L" \"" + widen(elfPath) + L"\"";
        cmd += L" --soul-event=" + widen(character) + L":" + std::to_wstring(door);
        PROCESS_INFORMATION pi{};
        const std::wstring cwd = widen(session->dir.string());
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env.data(), cwd.c_str(), &si, &pi);
        if (log != INVALID_HANDLE_VALUE)
            CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE)
            CloseHandle(nul);
        if (!ok)
        {
            error = "could not start the event process (error " + std::to_string(GetLastError()) + ")";
            return nullptr;
        }
        if (!AssignProcessToJobObject(session->job, pi.hProcess))
        {
            // without the job the "never orphaned" guarantee is gone: do not run
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            error = "could not place the event process in its job (error " + std::to_string(GetLastError()) + ")";
            return nullptr;
        }
        session->process = pi.hProcess;
        session->pid = pi.dwProcessId;
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        return session;
    }
#else
    int removeStaleFolders() { return 0; }

    std::unique_ptr<Session> start(const std::string &, const std::string &, const std::string &, int, const std::string &, const std::string &,
                                   const std::vector<std::pair<std::string, std::string>> &, std::string &error)
    {
        error = "event sessions are only available on Windows";
        return nullptr;
    }
#endif
}
