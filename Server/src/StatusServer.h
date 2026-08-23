#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace crossover {

// Minimal HTTP/1.1 endpoint so Railway's healthcheck has something to hit and so
// players can see who is online without joining. Deliberately tiny: no routing
// library, no keep-alive, one request per connection.
//
//   GET /health  -> 200 "ok"
//   GET /        -> 200 application/json, the status snapshot
class StatusServer {
public:
    using JsonProvider = std::function<std::string()>;

    StatusServer(unsigned short port, JsonProvider provider);
    ~StatusServer();

    bool Start();
    void Stop();

private:
    void ServeLoop();

    unsigned short port_;
    JsonProvider provider_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace crossover
