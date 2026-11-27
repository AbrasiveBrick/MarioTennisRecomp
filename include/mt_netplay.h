#pragma once
// Online play session control for the launcher (src/game/mt_netplay.cpp).
#include <string>

namespace mt_net {
    enum class SessionState { Idle, Connecting, Connected, Failed };

    struct SessionOptions {
        bool host = true;
        std::string address;  // join only
        int port = 7777;
        int delay = 2;        // host only, in game frames (30 per second)
        std::string pad;      // "" = keyboard and every controller, "keyboard", or a controller number from 1
    };

    // True when MT_NET_HOST or MT_NET_JOIN set up a session from the environment.
    bool env_configured();
    // Connects in the background; poll session_state() until Connected (then start the game) or Failed.
    void begin_session(const SessionOptions& options);
    void cancel_session();
    SessionState session_state();
    // The latest status or error message, for display.
    std::string session_status();
}
