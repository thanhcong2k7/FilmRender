#include "ProcessRunner.h"

#include <cstring>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace filmrender {

namespace {

class LineAccumulator {
public:
    explicit LineAccumulator(const ProcessRunner::OutputCallback& cb) : cb_(cb) {}

    void Feed(const char* data, std::size_t len) {
        if (!cb_) {
            return;
        }
        for (std::size_t i = 0; i < len; ++i) {
            const char ch = data[i];
            if (ch == '\n') {
                if (lastWasCr_) {
                    // '\r\n' -> treat as normal newline (if anything accumulated since \r, emit it)
                    lastWasCr_ = false;
                    if (!buf_.empty()) {
                        cb_(buf_, false);
                        buf_.clear();
                    }
                    continue;
                }
                cb_(buf_, false);
                buf_.clear();
            } else if (ch == '\r') {
                if (!buf_.empty()) {
                    cb_(buf_, true);
                    buf_.clear();
                }
                lastWasCr_ = true;
            } else {
                lastWasCr_ = false;
                buf_.push_back(ch);
            }
        }
    }

    void Flush() {
        if (cb_ && !buf_.empty()) {
            cb_(buf_, false);
            buf_.clear();
        }
    }

private:
    const ProcessRunner::OutputCallback& cb_;
    std::string buf_;
    bool lastWasCr_ = false;
};

bool NeedsQuotes(const std::string& s) {
    if (s.empty()) {
        return true;
    }
    for (char ch : s) {
        if (ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' || ch == '\\') {
            return true;
        }
    }
    return false;
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], needed);
    return wide;
}

std::wstring QuoteWin32Arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (auto it = arg.begin(); ; ++it) {
        std::size_t numBackslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++numBackslashes;
        }
        if (it == arg.end()) {
            out.append(numBackslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            out.append(numBackslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(numBackslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}
#endif

} // namespace

ProcessRunner::~ProcessRunner() {
    Cancel();
}

std::string ProcessRunner::FormatCommand(const std::string& executable,
                                         const std::vector<std::string>& args) {
    std::ostringstream oss;
    auto appendQuoted = [&oss](const std::string& tok) {
        if (!NeedsQuotes(tok)) {
            oss << tok;
            return;
        }
        oss << '"';
        for (char ch : tok) {
            if (ch == '"' || ch == '\\') {
                oss << '\\';
            }
            oss << ch;
        }
        oss << '"';
    };

    appendQuoted(executable);
    for (const std::string& arg : args) {
        oss << ' ';
        appendQuoted(arg);
    }
    return oss.str();
}

#ifdef _WIN32

bool ProcessRunner::Run(const std::string& executable,
                        const std::vector<std::string>& args,
                        const OutputCallback& onOutput,
                        int& exitCodeOut,
                        std::string& errorOut) {
    cancelled_.store(false);
    exitCodeOut = -1;
    errorOut.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        errorOut = "Failed to create output pipe (Win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        errorOut = "Failed to configure pipe handle.";
        return false;
    }

    // Open NUL for child stdin so ffmpeg never waits on interactive prompts
    HANDLE nulInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring cmdLine = QuoteWin32Arg(Utf8ToWide(executable));
    for (const std::string& a : args) {
        cmdLine.push_back(L' ');
        cmdLine += QuoteWin32Arg(Utf8ToWide(a));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = (nulInput != INVALID_HANDLE_VALUE) ? nulInput : nullptr;

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        nullptr,
        &cmdLine[0],
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    CloseHandle(writePipe);
    if (nulInput != INVALID_HANDLE_VALUE) {
        CloseHandle(nulInput);
    }

    if (!created) {
        const DWORD err = GetLastError();
        CloseHandle(readPipe);
        errorOut = "Failed to start '" + executable + "' (Win32 error " + std::to_string(err) + ")";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(procMutex_);
        activeProcessHandle_ = pi.hProcess;
    }

    LineAccumulator acc(onOutput);
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        acc.Feed(buffer, static_cast<std::size_t>(bytesRead));
    }
    acc.Flush();
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    exitCodeOut = static_cast<int>(code);

    {
        std::lock_guard<std::mutex> lock(procMutex_);
        activeProcessHandle_ = nullptr;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (cancelled_.load()) {
        errorOut = "Process cancelled by user.";
        return false;
    }
    if (exitCodeOut != 0) {
        errorOut = "Process exited with code " + std::to_string(exitCodeOut);
        return false;
    }
    return true;
}

void ProcessRunner::Cancel() {
    cancelled_.store(true);
    std::lock_guard<std::mutex> lock(procMutex_);
    if (activeProcessHandle_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(activeProcessHandle_), 1);
    }
}

#else

bool ProcessRunner::Run(const std::string& executable,
                        const std::vector<std::string>& args,
                        const OutputCallback& onOutput,
                        int& exitCodeOut,
                        std::string& errorOut) {
    cancelled_.store(false);
    exitCodeOut = -1;
    errorOut.clear();

    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) != 0) {
        errorOut = std::string("Failed to create pipe: ") + std::strerror(errno);
        return false;
    }

    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(executable);
    for (const auto& a : args) {
        storage.push_back(a);
    }

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& s : storage) {
        argv.push_back(&s[0]);
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        const int err = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errorOut = std::string("fork() failed: ") + std::strerror(err);
        return false;
    }

    if (pid == 0) {
        // Child process
        setpgid(0, 0);

        // Redirect stdin to /dev/null so ffmpeg never hangs waiting for input
        const int devNull = open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            if (devNull > STDERR_FILENO) {
                close(devNull);
            }
        }

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        execvp(executable.c_str(), argv.data());
        const int err = errno;
        const std::string msg = "Failed to execute '" + executable + "': " + std::strerror(err) + "\n";
        const ssize_t ignored = write(STDERR_FILENO, msg.data(), msg.size());
        (void)ignored;
        _exit(127);
    }

    // Parent process
    close(pipefd[1]);

    {
        std::lock_guard<std::mutex> lock(procMutex_);
        activePid_ = static_cast<long>(pid);
    }

    LineAccumulator acc(onOutput);
    char buffer[4096];
    for (;;) {
        const ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            acc.Feed(buffer, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
    }
    acc.Flush();
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(procMutex_);
        activePid_ = -1;
    }

    if (WIFEXITED(status)) {
        exitCodeOut = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exitCodeOut = 128 + WTERMSIG(status);
    }

    if (cancelled_.load()) {
        errorOut = "Process cancelled by user.";
        return false;
    }
    if (exitCodeOut != 0) {
        errorOut = "Process exited with code " + std::to_string(exitCodeOut);
        return false;
    }
    return true;
}

void ProcessRunner::Cancel() {
    cancelled_.store(true);
    long pidToKill = -1;
    {
        std::lock_guard<std::mutex> lock(procMutex_);
        pidToKill = activePid_;
    }
    if (pidToKill > 0) {
        // Send SIGTERM to the child's process group, then SIGKILL if it doesn't exit quickly.
        kill(-static_cast<pid_t>(pidToKill), SIGTERM);
        for (int i = 0; i < 15; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> lock(procMutex_);
            if (activePid_ != pidToKill) {
                return;
            }
        }
        kill(-static_cast<pid_t>(pidToKill), SIGKILL);
    }
}

#endif

} // namespace filmrender
