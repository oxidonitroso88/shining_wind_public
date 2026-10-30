#ifndef PS2X_ERROR_LOG_H
#define PS2X_ERROR_LOG_H

// Minimal, size-limited error log for the distributed build (no development logging is switched on):
//   <data-dir>/logs/ShiningWind.log            the current run (at most kMaxBytes)
//   <data-dir>/logs/ShiningWind.previous.log   the run before (the old file is replaced at each start)
// It records a short header, the first kHeadLines lines of output (start-up context) and, after that, only lines that look like
// problems (error / fail / fatal / exception / missing-target / terminate ...). Everything still goes to the original console or
// file as before. A crash outside the emulation (access violation, ...) is written with its code and address.
// Only installed when a per-user data folder is in use (--disc / double click); the positional development launch is unchanged.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ps2x::errlog
{
    inline constexpr size_t kMaxBytes = 1u << 20; // 1 MiB per file, two files at most
    inline constexpr int kHeadLines = 60;
    inline constexpr size_t kMaxLine = 400;
    inline constexpr int kMaxRepeats = 5;

    class Sink
    {
    public:
        static Sink &instance()
        {
            static Sink sink;
            return sink;
        }

        bool open(const std::filesystem::path &dataDir)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::error_code ec;
            const std::filesystem::path dir = dataDir / "logs";
            std::filesystem::create_directories(dir, ec);
            if (ec)
                return false;
            const std::filesystem::path current = dir / "ShiningWind.log";
            const std::filesystem::path previous = dir / "ShiningWind.previous.log";
            if (std::filesystem::exists(current, ec))
            {
                std::filesystem::remove(previous, ec);
                std::filesystem::rename(current, previous, ec);
            }
            file_.open(current, std::ios::binary | std::ios::trunc);
            return static_cast<bool>(file_);
        }

        bool active() const { return file_.is_open(); }

        void header(const std::string &text)
        {
            writeRaw(text);
        }

        void line(const std::string &raw)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!file_.is_open() || full_)
                return;
            const bool interesting = looksLikeProblem(raw);
            if (headCount_ < kHeadLines)
                ++headCount_;
            else if (!interesting)
                return;
            // The same message (numbers ignored) is recorded a few times only, so a repeated line cannot fill the file.
            std::string key;
            for (char c : raw)
                if (!std::isdigit(static_cast<unsigned char>(c)) || key.empty() || key.back() != '#')
                    key.push_back(std::isdigit(static_cast<unsigned char>(c)) ? '#' : c);
            if (seen_.size() < 4000 || seen_.count(key))
            {
                const int count = ++seen_[key];
                if (count == kMaxRepeats + 1)
                {
                    writeLocked("[the previous message repeats; further copies are not recorded]\n");
                    return;
                }
                if (count > kMaxRepeats + 1)
                    return;
            }
            std::string text = raw.size() > kMaxLine ? raw.substr(0, kMaxLine) + "..." : raw;
            writeLocked(text + "\n");
        }

        // Called from the crash handler: no locks that may be held by the crashing thread.
        void writeCrash(const std::string &text)
        {
            if (!file_.is_open())
                return;
            file_ << text;
            file_.flush();
        }

    private:
        static bool looksLikeProblem(const std::string &line)
        {
            std::string lower = line;
            for (char &c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower.find("geterror") != std::string::npos) // the CDVD query "GetError" is routine, not a problem
                return false;
            static const char *words[] = {"error", "fail", "fatal", "exception", "missing-target", "terminate", "cannot", "unable", "abort", "unhandled", "not supported"};
            for (const char *w : words)
                if (lower.find(w) != std::string::npos)
                    return true;
            return false;
        }

        void writeRaw(const std::string &text)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (file_.is_open() && !full_)
                writeLocked(text);
        }

        void writeLocked(const std::string &text)
        {
            if (written_ + text.size() > kMaxBytes)
            {
                static const char note[] = "[log full: further messages are not recorded]\n";
                file_.write(note, sizeof(note) - 1);
                file_.flush();
                full_ = true;
                return;
            }
            file_.write(text.data(), static_cast<std::streamsize>(text.size()));
            file_.flush();
            written_ += text.size();
        }

        std::mutex mutex_;
        std::ofstream file_;
        size_t written_ = 0;
        int headCount_ = 0;
        std::unordered_map<std::string, int> seen_;
        bool full_ = false;
    };

    // Copies every line to the sink (filtered there) and forwards the characters to the original buffer, if one is usable.
    class TeeBuf : public std::streambuf
    {
    public:
        TeeBuf(std::streambuf *original, bool forward) : original_(original), forward_(forward && original != nullptr) {}

    protected:
        int_type overflow(int_type ch) override
        {
            if (traits_type::eq_int_type(ch, traits_type::eof()))
                return traits_type::not_eof(ch);
            const char c = traits_type::to_char_type(ch);
            feed(&c, 1);
            if (forward_)
                original_->sputc(c);
            return ch;
        }

        std::streamsize xsputn(const char *s, std::streamsize n) override
        {
            feed(s, static_cast<size_t>(n));
            if (forward_)
                original_->sputn(s, n);
            return n;
        }

        int sync() override { return forward_ ? original_->pubsync() : 0; }

    private:
        void feed(const char *s, size_t n)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t i = 0; i < n; ++i)
            {
                if (s[i] == '\n')
                {
                    if (!line_.empty() && line_.back() == '\r')
                        line_.pop_back();
                    Sink::instance().line(line_);
                    line_.clear();
                }
                else if (line_.size() < 4096)
                {
                    line_.push_back(s[i]);
                }
            }
        }

        std::streambuf *original_;
        bool forward_;
        std::mutex mutex_;
        std::string line_;
    };

#if defined(_WIN32)
    inline LONG WINAPI crashFilter(EXCEPTION_POINTERS *info)
    {
        char text[256];
        const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
        const void *address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module))
            GetModuleFileNameA(module, name, MAX_PATH);
        const char *base = std::strrchr(name, '\\');
        std::snprintf(text, sizeof(text), "[crash] unhandled exception code=0x%08lX address=%p module=%s offset=0x%llX\n", static_cast<unsigned long>(code), address,
                      base ? base + 1 : name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
        Sink::instance().writeCrash(text);
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

    // Starts the log under <dataDir>/logs. `header` describes this run (version, disc, folders). Returns false if the folder is unusable.
    inline bool install(const std::filesystem::path &dataDir, const std::string &header, bool forwardToOriginal)
    {
        static std::unique_ptr<TeeBuf> errBuf, outBuf;
        if (dataDir.empty() || !Sink::instance().open(dataDir))
            return false;
        char stamp[32] = "";
        const std::time_t now = std::time(nullptr);
        if (const std::tm *tm = std::localtime(&now))
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", tm);
        Sink::instance().header(std::string("ShiningWind log ") + stamp + "\n" + header + "\n");
        static std::streambuf *originalErr = std::cerr.rdbuf();
        static std::streambuf *originalOut = std::cout.rdbuf();
        errBuf = std::make_unique<TeeBuf>(originalErr, forwardToOriginal);
        outBuf = std::make_unique<TeeBuf>(originalOut, forwardToOriginal);
        std::cerr.rdbuf(errBuf.get());
        std::cout.rdbuf(outBuf.get());
        std::atexit([] { // the tee buffers are statics: give the streams their own buffers back before those are destroyed
            std::cerr.rdbuf(originalErr);
            std::cout.rdbuf(originalOut);
        });
#if defined(_WIN32)
        SetUnhandledExceptionFilter(crashFilter);
#endif
        return true;
    }
}

#endif
