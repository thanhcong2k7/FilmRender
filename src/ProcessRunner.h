#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace filmrender {

class ProcessRunner {
public:
    /// Callback invoked for each line or carriage-return progress update.
    /// `isProgressUpdate` is true when terminated by '\r' (e.g. ffmpeg -stats).
    using OutputCallback = std::function<void(const std::string& line, bool isProgressUpdate)>;

    ProcessRunner() = default;
    ~ProcessRunner();

    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    /// Run `executable` with `args` synchronously (designed to be called on a worker thread).
    /// Returns true if process started and exited with code 0; false otherwise.
    bool Run(const std::string& executable,
             const std::vector<std::string>& args,
             const OutputCallback& onOutput,
             int& exitCodeOut,
             std::string& errorOut);

    /// Thread-safe cancellation of the currently running child process.
    void Cancel();

    /// Returns true if Cancel() was requested since the last Run().
    bool WasCancelled() const { return cancelled_.load(); }

    /// Format executable + args into a readable command string for logging.
    static std::string FormatCommand(const std::string& executable,
                                     const std::vector<std::string>& args);

private:
    std::mutex procMutex_;
    std::atomic<bool> cancelled_{false};

#ifdef _WIN32
    void* activeProcessHandle_ = nullptr; // HANDLE
#else
    long activePid_ = -1; // pid_t
#endif
};

} // namespace filmrender
