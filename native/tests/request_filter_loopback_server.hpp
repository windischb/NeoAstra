#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace neo_request_filter_probe {

inline void server_check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Probe server failure: %s\n", message);
        std::abort();
    }
}

struct counts {
    unsigned connections{};
    unsigned requests{};
    unsigned upgrades{};
    unsigned long long bytes{};
};

inline bool no_request(counts value) {
    // Plaintext HTTP: every received byte is application data. An empty
    // connection is diagnostic, not a failure of the request contract.
    return value.bytes == 0 && value.requests == 0 && value.upgrades == 0;
}

inline counts since(counts before, counts after) {
    return {after.connections - before.connections, after.requests - before.requests,
        after.upgrades - before.upgrades, after.bytes - before.bytes};
}

// Nonblocking clients keep an idle preconnect from hiding subsequent requests.
class loopback_server final {
public:
    explicit loopback_server(std::string redirect = {}, bool ipv6 = false)
        : _redirect(std::move(redirect)), _ipv6(ipv6) {
        _listener = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
        server_check(_listener >= 0, "socket");
        if (ipv6) {
            int only = 1;
            server_check(setsockopt(_listener, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(only)) == 0, "IPv6-only listener");
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_addr = in6addr_loopback;
            server_check(bind(_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind IPv6 loopback");
            socklen_t size = sizeof(address);
            server_check(getsockname(_listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "IPv6 getsockname");
            _port = ntohs(address.sin6_port);
        } else {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            server_check(bind(_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind loopback");
            socklen_t size = sizeof(address);
            server_check(getsockname(_listener, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
            _port = ntohs(address.sin_port);
        }
        server_check(listen(_listener, 16) == 0, "listen");
        server_check(fcntl(_listener, F_SETFL, O_NONBLOCK) == 0, "nonblocking listener");
        _thread = std::thread([this] { serve(); });
    }

    ~loopback_server() {
        _stopping.store(true);
        _thread.join();
        close(_listener);
    }
    loopback_server(const loopback_server&) = delete;
    loopback_server& operator=(const loopback_server&) = delete;

    uint16_t port() const { return _port; }
    unsigned worker_scripts() const { return _worker_scripts.load(); }
    std::string origin(const char* host = nullptr) const {
        return "http://" + std::string(host ? host : _ipv6 ? "[::1]" : "127.0.0.1") + ":" + std::to_string(_port);
    }
    counts snapshot() {
        // Server observation only, not a browser-process update barrier.
        const auto sample = ++_samples;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (_sampled.load() < sample) {
            server_check(std::chrono::steady_clock::now() < deadline, "server observation");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {_connections.load(), _requests.load(), _upgrades.load(), _bytes.load()};
    }

private:
    struct client { int fd; std::string data; };
    int _listener{-1};
    uint16_t _port{};
    const std::string _redirect;
    const bool _ipv6;
    std::atomic<bool> _stopping{};
    std::atomic<unsigned> _connections{}, _requests{}, _upgrades{};
    std::atomic<unsigned> _worker_scripts{};
    std::atomic<unsigned long long> _bytes{};
    std::atomic<unsigned> _samples{}, _sampled{};
    std::thread _thread;

    void serve() {
        std::vector<client> clients;
        while (!_stopping.load()) {
            const auto sample = _samples.load();
            pollfd listener{_listener, POLLIN, 0};
            const int ready = poll(&listener, 1, 10);
            server_check(ready >= 0 || errno == EINTR, "poll listener");
            while (ready > 0) {
                const int fd = accept(_listener, nullptr, nullptr);
                if (fd >= 0) {
                    ++_connections;
#ifdef SO_NOSIGPIPE
                    int yes = 1;
                    server_check(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0, "SO_NOSIGPIPE");
#endif
                    timeval timeout{1, 0};
                    server_check(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0, "send timeout");
                    clients.push_back({fd, {}});
                } else {
                    server_check(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR, "accept");
                    break;
                }
            }
            for (auto it = clients.begin(); it != clients.end();) {
                char buffer[8192];
                const auto length = recv(it->fd, buffer, sizeof(buffer), MSG_DONTWAIT);
                if (length > 0) {
                    _bytes.fetch_add(static_cast<unsigned long long>(length));
                    it->data.append(buffer, static_cast<size_t>(length));
                }
                const bool complete = it->data.find("\r\n\r\n") != std::string::npos;
                if (complete) {
                    ++_requests;
                    auto lower = it->data;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
                    });
                    const bool upgrade = lower.find("\r\nupgrade: websocket\r\n") != std::string::npos;
                    if (upgrade) ++_upgrades;
                    const auto first_space = it->data.find(' ');
                    const auto second_space = it->data.find(' ', first_space + 1);
                    const auto path = it->data.substr(first_space + 1, second_space - first_space - 1);
                    std::string redirect_code;
                    if (!_redirect.empty()) {
                        if (path == "/redirect") redirect_code = "302";
                        for (const char* code : {"301", "302", "303", "307", "308"})
                            if (path == "/redirect/" + std::string(code) || path == "/chain/" + std::string(code))
                                redirect_code = code;
                    }
                    const bool frame = !_redirect.empty() && path == "/frame";
                    const auto script_path = path.substr(0, path.find('?'));
                    const bool dedicated = !_redirect.empty() && script_path == "/dedicated-worker.js";
                    const bool shared = !_redirect.empty() && script_path == "/shared-worker.js";
                    const bool service = !_redirect.empty() && script_path == "/service-worker.js";
                    const bool worker = dedicated || shared || service;
                    if (worker) ++_worker_scripts;
                    std::string body = "<!doctype html><meta charset=utf-8><title>Request filter probe</title><body>Probe</body>" +
                        (frame ? "<script>fetch('" + _redirect + "/frame-fetch').then(()=>{window.probeFetchResult='load';parent.postMessage('load','*')},()=>{window.probeFetchResult='error';parent.postMessage('error','*')});</script>" : "");
                    if (dedicated)
                        body = "onmessage=()=>fetch('" + _redirect + "/worker-fetch',{cache:'no-store'}).then("
                            "()=>postMessage('load'),()=>postMessage('error'));";
                    else if (shared || service) {
                        body = "const instance=Date.now()+'-'+Math.random();"
                            "async function answer(e){const p=e.ports[0];if(!p)return;"
                            "let result='ready:'+instance;"
                            "if(e.data.cmd==='fetch'){try{await fetch('" + _redirect +
                            "/worker-fetch?case='+encodeURIComponent(e.data.label),{cache:'no-store'});result='load';}"
                            "catch(_){result='error';}}"
                            "if(e.data.cmd==='stop')result='stopped';"
                            "p.postMessage(result);p.close();";
                        if (shared) body += "if(e.data.cmd==='stop')self.close();";
                        body += "}";
                        body += shared ?
                            "onconnect=e=>{const p=e.ports[0];p.onmessage=answer;p.start();};" :
                            "oninstall=e=>e.waitUntil(self.skipWaiting());"
                            "onactivate=e=>e.waitUntil(self.clients.claim());"
                            "onmessage=e=>e.waitUntil(answer(e));";
                    }
                    const std::string response =
                        (!redirect_code.empty() ? "HTTP/1.1 " + redirect_code + " Redirect\r\nLocation: " + _redirect +
                            (path.starts_with("/chain/") ? "/redirect/" + redirect_code : "/target") + "\r\n" :
                            upgrade ? "HTTP/1.1 400 Bad Request\r\n" : "HTTP/1.1 200 OK\r\n") +
                        std::string("Content-Type: ") + (worker ? "text/javascript" : "text/html") +
                        "\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body;
                    size_t sent = 0;
                    while (sent < response.size()) {
#ifdef MSG_NOSIGNAL
                        constexpr int flags = MSG_NOSIGNAL;
#else
                        constexpr int flags = 0;
#endif
                        const auto n = send(it->fd, response.data() + sent, response.size() - sent, flags);
                        if (n <= 0) break; // The browser may cancel an unsuitable image/script response.
                        sent += static_cast<size_t>(n);
                    }
                }
                if (complete || length == 0 || it->data.size() > 32768 ||
                    (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    close(it->fd);
                    it = clients.erase(it);
                } else ++it;
            }
            _sampled.store(sample);
        }
        for (const auto& client : clients) close(client.fd);
    }
};

} // namespace neo_request_filter_probe
