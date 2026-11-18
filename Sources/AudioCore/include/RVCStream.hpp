#pragma once
#include <array>
#include <atomic>
#include <Accelerate/Accelerate.h>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <crt_externs.h>

namespace mv {
// Control queue owns process lifetime. Only the cleanup worker exchanges PCM.
class RVCStream {
    int fd = -1;
    pid_t child = -1;
    uint64_t sent = 0, received = 0;
    bool primed = false;
    double chunkMeanSquareSum = 0;
    unsigned meterBlocks = 0;
    unsigned bufferTicks = 0;
    uint64_t silenceThrough = 0;
    std::array<double, 400> origins{};
public:
    unsigned blocksPerChunk = 0;
    std::atomic<double> inputRMS{0}, displayRMS{0};
    std::string python, script, config;
    bool enabled() const { return !config.empty(); }

    void closeStream() {
        inputRMS.store(0, std::memory_order_relaxed);
        displayRMS.store(0, std::memory_order_relaxed);
        if (fd >= 0) { close(fd); fd = -1; }
        if (child > 0) {
            kill(child, SIGKILL);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
            child = -1;
        }
    }
    ~RVCStream() { closeStream(); }
    void start() {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) throw std::runtime_error("Could not open local voice audio pipe.");
        fd = sockets[0];
        int one = 1, capacity = 256 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        for (int s : sockets) {
            setsockopt(s, SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity));
            setsockopt(s, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity));
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, fd);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, sockets[1]);
        posix_spawnattr_t attrs;
        posix_spawnattr_init(&attrs);
        posix_spawnattr_setflags(&attrs, POSIX_SPAWN_CLOEXEC_DEFAULT);
        posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
        const char *args[] = {python.c_str(), "-I", "-B", script.c_str(), config.c_str(), nullptr};
        pid_t pid;
        int err = posix_spawn(&pid, python.c_str(), &actions, &attrs, const_cast<char**>(args), *_NSGetEnviron());
        posix_spawnattr_destroy(&attrs);
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (err) throw std::runtime_error("Could not launch the local RVC runtime.");
        child = pid;
        pollfd p{fd, POLLIN, 0};
        std::string line;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (line.size() < 512 && std::chrono::steady_clock::now() < deadline) {
            int ready = poll(&p, 1, 250);
            if (ready < 0 && errno != EINTR) break;
            if (ready <= 0) continue;
            char c;
            if (recv(fd, &c, 1, 0) != 1 || c == '\n') break;
            line.push_back(c);
        }
        if (line != "OK") throw std::runtime_error(line.empty() ? "Voice runtime failed to load the selected model." : line);
        sent = received = 0;
        chunkMeanSquareSum = 0;
        meterBlocks = 0;
        primed = false;
        bufferTicks = 0;
        silenceThrough = 0;
    }

    bool exchange(float *block, double &origin, bool muted = false) {
        constexpr size_t bytes = 480 * sizeof(float);
        if (muted) {
            std::fill_n(block, 480, 0.0f);
            silenceThrough = sent + 5; // Account for synthesis overlap on unmute.
        }
        if (sent - received >= origins.size() || send(fd, block, bytes, MSG_DONTWAIT) != bytes) return false;
        // The worker gates one complete chunk of these post-cleanup, post-mute samples.
        float meanSquare;
        vDSP_measqv(block, 1, &meanSquare, 480);
        const double level = std::sqrt(meanSquare);
        const double previous = displayRMS.load(std::memory_order_relaxed);
        displayRMS.store(previous + (level - previous) * (level > previous ? 0.8 : 0.12),
                         std::memory_order_relaxed);
        chunkMeanSquareSum += meanSquare;
        if (++meterBlocks == blocksPerChunk) {
            inputRMS.store(std::sqrt(chunkMeanSquareSum / meterBlocks), std::memory_order_relaxed);
            chunkMeanSquareSum = 0;
            meterBlocks = 0;
        }
        origins[sent++ % origins.size()] = origin;
        int available = 0;
        if (ioctl(fd, FIONREAD, &available)) return false;
        // Allow jitter in inference time between chunks before starting playout.
        if (!primed && available >= int(bytes) && ++bufferTicks >= 12) primed = true;
        if (primed && available >= int(bytes)) {
            if (recv(fd, block, bytes, MSG_DONTWAIT) != bytes) return false;
            origin = origins[received % origins.size()];
            if (received++ < silenceThrough) std::fill_n(block, 480, 0.0f);
        } else {
            if (available < int(bytes)) { primed = false; bufferTicks = 0; }
            std::fill_n(block, 480, 0.0f);
            origin = 0;
        }
        return true;
    }
};
}
