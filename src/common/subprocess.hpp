// POSIX process helpers shared by the contest tooling: launching the solver
// binaries / graphviz as children, with wall-clock deadlines and optional
// stdout capture. fork/exec based (Linux + macOS).
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace proc {

using Clock = std::chrono::steady_clock;

inline std::vector<char*> toArgv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    return argv;
}

// A non-blocking child process: stdout/stderr optionally redirected to files
// (or /dev/null), stdin closed. Used for long-running solver launches where
// progress is tracked via a separate --status-file rather than stdout.
class Child {
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& o) noexcept { *this = std::move(o); }
    Child& operator=(Child&& o) noexcept {
        if (this != &o) { pid_ = o.pid_; done_ = o.done_; status_ = o.status_; o.pid_ = -1; }
        return *this;
    }
    ~Child() { if (pid_ > 0 && !done_) killIfRunning(); }

    bool valid() const { return pid_ > 0; }
    pid_t pid() const { return pid_; }

    static Child spawn(const std::vector<std::string>& args,
                       const std::string& cwd = "",
                       const std::string& stdoutPath = "",
                       const std::string& stderrPath = "") {
        Child c;
        auto argv = toArgv(args);
        pid_t pid = fork();
        if (pid < 0) throw std::runtime_error("fork failed");
        if (pid == 0) {
            // child
            if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
            auto openTrunc = [](const std::string& path) {
                return open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            };
            // Combined-log case (stdout==stderr path): open once and dup2 both
            // fds to it so writes interleave correctly (matches Python's
            // stdout=log, stderr=STDOUT). Separate paths get independent fds.
            if (!stdoutPath.empty() && stdoutPath == stderrPath) {
                int f = openTrunc(stdoutPath);
                if (f >= 0) { dup2(f, STDOUT_FILENO); dup2(f, STDERR_FILENO); close(f); }
            } else {
                auto redirect = [&](const std::string& path, int fd) {
                    if (path.empty()) return;
                    int f = openTrunc(path);
                    if (f >= 0) { dup2(f, fd); close(f); }
                };
                redirect(stdoutPath, STDOUT_FILENO);
                redirect(stderrPath, STDERR_FILENO);
            }
            execvp(argv[0], argv.data());
            _exit(127); // exec failed
        }
        c.pid_ = pid;
        return c;
    }

    // Returns true and fills exitCode once the child has exited (WNOHANG poll).
    bool poll(int& exitCode) {
        if (done_) { exitCode = status_; return true; }
        int status = 0;
        pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r == 0) return false;
        done_ = true;
        status_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        exitCode = status_;
        return true;
    }

    int wait() {
        if (done_) return status_;
        int status = 0;
        waitpid(pid_, &status, 0);
        done_ = true;
        status_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return status_;
    }

    void killIfRunning(int sig = SIGTERM) {
        if (pid_ <= 0 || done_) return;
        ::kill(pid_, sig);
        int status = 0;
        waitpid(pid_, &status, 0);
        done_ = true;
        status_ = -1;
    }

private:
    pid_t pid_ = -1;
    bool  done_ = false;
    int   status_ = -1;
};

struct CapturedResult {
    bool        ok = false;      // exited with status 0 before timeout
    bool        timedOut = false;
    int         exitCode = -1;
    std::string stdoutData;
    std::string stderrData;
};

struct LoggedResult {
    int    exitCode = -9;
    double wallSec  = 0.0;
    bool   timedOut = false;
};

// Runs a (potentially long) child with combined stdout+stderr redirected to
// logPath, killing it if it runs past timeoutSec (infinity = no deadline).
// Mirrors the Python orchestrator's `subprocess.run(cmd, stdout=log,
// stderr=STDOUT, timeout=to)` + SIGKILL-on-TimeoutExpired backstop.
inline LoggedResult runLogged(const std::vector<std::string>& args,
                               const std::string& logPath,
                               double timeoutSec = -1.0,
                               const std::string& cwd = "") {
    auto t0 = Clock::now();
    LoggedResult res;
    Child c = Child::spawn(args, cwd, logPath, logPath);
    while (true) {
        int code;
        if (c.poll(code)) { res.exitCode = code; break; }
        double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        if (timeoutSec >= 0.0 && elapsed >= timeoutSec) {
            c.killIfRunning(SIGKILL);
            res.timedOut = true;
            res.exitCode = -9;
            FILE* f = fopen(logPath.c_str(), "a");
            if (f) {
                fprintf(f, "\n[orch] KILLED by budget backstop after %.0fs (limit %.0fs)\n",
                        elapsed, timeoutSec);
                fclose(f);
            }
            break;
        }
        struct timespec ts{0, 20 * 1000 * 1000}; // 20ms poll interval
        nanosleep(&ts, nullptr);
    }
    res.wallSec = std::chrono::duration<double>(Clock::now() - t0).count();
    return res;
}

// Synchronous run with stdin data and a wall-clock timeout (seconds).
// Used for short-lived helper processes (graphviz engines, `make`).
inline CapturedResult runCaptured(const std::vector<std::string>& args,
                                   const std::string& stdinData,
                                   double timeoutSec,
                                   const std::string& cwd = "") {
    CapturedResult res;
    int inPipe[2], outPipe[2], errPipe[2];
    if (pipe(inPipe) || pipe(outPipe) || pipe(errPipe)) {
        throw std::runtime_error("pipe() failed");
    }
    auto argv = toArgv(args);
    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(inPipe[0]); close(outPipe[1]); close(errPipe[1]);
    // Feed stdin then close so the child sees EOF.
    {
        size_t off = 0;
        fcntl(inPipe[1], F_SETFL, O_NONBLOCK);
        // Best effort non-blocking write; graphviz inputs are small (<few MB).
        while (off < stdinData.size()) {
            ssize_t w = write(inPipe[1], stdinData.data() + off, stdinData.size() - off);
            if (w > 0) { off += (size_t)w; continue; }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pfd{inPipe[1], POLLOUT, 0};
                poll(&pfd, 1, 50);
                continue;
            }
            break;
        }
    }
    close(inPipe[1]);

    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    auto deadline = Clock::now() + std::chrono::duration<double>(timeoutSec);
    bool exited = false;
    int status = 0;
    char buf[65536];
    while (true) {
        struct pollfd pfds[2] = {
            {outPipe[0], POLLIN, 0},
            {errPipe[0], POLLIN, 0},
        };
        auto remain = deadline - Clock::now();
        int remainMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(remain).count();
        if (remainMs < 0) remainMs = 0;
        poll(pfds, 2, remainMs > 200 ? 200 : (remainMs < 1 ? 1 : remainMs));

        ssize_t n;
        while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) res.stdoutData.append(buf, n);
        while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) res.stderrData.append(buf, n);

        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { exited = true; break; }
        if (Clock::now() >= deadline) break;
    }
    // Drain anything left after exit.
    if (exited) {
        ssize_t n;
        while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) res.stdoutData.append(buf, n);
        while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) res.stderrData.append(buf, n);
    }
    close(outPipe[0]); close(errPipe[0]);

    if (!exited) {
        ::kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        res.timedOut = true;
        res.ok = false;
        return res;
    }
    res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    res.ok = (res.exitCode == 0);
    return res;
}

} // namespace proc
