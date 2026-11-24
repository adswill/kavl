// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// KAVL app rig control (app/engine/rig.h) against Hamlib's simulated rig (model 1): KAVL starts rigctld, sets the frequency, keys and
// releases PTT, and stops rigctld again; then the same through a rigctld that is already running. Skips when Hamlib is not installed.
#include "rig.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>

extern char** environ;
static int fails = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            fails++;                                                      \
        }                                                                 \
    } while (0)

static bool waitFor(RigControl& r, bool (*ok)(const RigControl::Status&), double sec) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < sec) {
        if (ok(r.status())) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static void exercise(RigControl& rig, const char* how) {
    // asked for before the connection is up (as the app does at start): it must still reach the rig
    rig.setFrequency(144.8e6);
    CHECK(waitFor(rig, [](const RigControl::Status& s) { return s.connected; }, 5.0));
    CHECK(waitFor(rig, [](const RigControl::Status& s) { return s.rigHz > 144.7e6 && s.rigHz < 144.9e6; }, 3.0));
    rig.setFrequency(433.25e6);
    CHECK(waitFor(rig, [](const RigControl::Status& s) { return s.rigHz > 433.2e6 && s.rigHz < 433.3e6; }, 3.0));
    rig.setPtt(true);
    CHECK(rig.waitPtt());
    CHECK(rig.status().ptt);
    rig.setPtt(false);
    CHECK(rig.waitPtt());
    CHECK(!rig.status().ptt);
    const auto s = rig.status();
    std::printf("%s: connected %d, rig at %.0f Hz, error '%s'\n", how, s.connected, s.rigHz, s.error.c_str());
}

int main() {
    const std::string exe = RigControl::findRigctld();
    if (exe.empty()) {
        std::printf("test_kavl_rig: Hamlib not installed, skipped\n");
        return 0;
    }
    {   // KAVL starts rigctld itself
        RigControl rig;
        RigSettings s;
        s.mode = 2;
        s.model = 1;
        rig.start(s);
        exercise(rig, "started by KAVL");
        rig.stop();
        CHECK(!rig.active());
    }
    {   // a rigctld that runs already (on a port of its own for the test)
        pid_t pid = -1;
        const char* argv[] = {exe.c_str(), "-m", "1", "-T", "127.0.0.1", "-t", "4590", nullptr};
        CHECK(posix_spawn(&pid, exe.c_str(), nullptr, nullptr, const_cast<char**>(argv), environ) == 0);
        RigControl rig;
        RigSettings s;
        s.mode = 1;
        s.port = 4590;
        rig.start(s);
        exercise(rig, "already running");
        rig.stop();
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
    {   // nothing listening: says so, and stop() returns at once
        RigControl rig;
        RigSettings s;
        s.mode = 1;
        s.port = 4591;
        rig.start(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        CHECK(!rig.status().connected && !rig.status().error.empty());
        const auto t0 = std::chrono::steady_clock::now();
        rig.stop();
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 2.0);
    }
    if (fails) {
        std::printf("test_kavl_rig: %d failures\n", fails);
        return 1;
    }
    std::printf("test_kavl_rig: all passed\n");
    return 0;
}
