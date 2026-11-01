#include "server.h"
#include "http_request.h"
#include "http_response.h"
#include "file_handler.h"
#include "logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include <chrono>
#include <thread>
#include <cerrno>
#include <algorithm>
#include <vector>
#include <fstream>
#include <regex>
#include <iomanip>

static size_t load_max_connections_from_config() {
    std::ifstream config_file("config.json");
    if (!config_file.is_open()) {
        return 10000;
    }
    
    std::string line;
    std::regex max_conn_regex(R"("max_connections"\s*:\s*(\d+))");
    std::smatch match;
    
    while (std::getline(config_file, line)) {
        if (std::regex_search(line, match, max_conn_regex)) {
            try {
                size_t max_conns = std::stoul(match[1].str());
                if (max_conns > 0 && max_conns <= 500000) {
                    return max_conns;
                }
            } catch (const std::exception&) {
                return 10000;
            }
        }
    }
    
    return 10000;
}

Server::Server(int port, const std::string& host, size_t thread_count)
    : server_fd_(-1), port_(port), host_(host), running_(false),
      max_connections_(load_max_connections_from_config()) {
    
    epoll_ = std::make_unique<EpollWrapper>();
    thread_pool_ = std::make_unique<ThreadPool>(thread_count);
    file_handler_ = std::make_unique<FileHandler>("./public", "index.html", true, 100);
    rate_limiter_ = std::make_unique<RateLimiter>(500.0, 1000.0, false);
    
    Logger::get_instance().init("./logs/access.log", "./logs/error.log", LogLevel::INFO);
}

Server::~Server() {
    stop();
}

bool Server::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd_ == -1) {
        Logger::get_instance().log_error("Failed to create server socket: " + std::string(strerror(errno)));
        return false;
    }
    
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        Logger::get_instance().log_warn("Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
    }
    
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == -1) {
        Logger::get_instance().log_warn("Failed to set SO_REUSEPORT: " + std::string(strerror(errno)));
    }
    
    if (setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1) {
        Logger::get_instance().log_warn("Failed to set TCP_NODELAY: " + std::string(strerror(errno)));
    }
    
    int buffer_size = 256 * 1024;
    setsockopt(server_fd_, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(server_fd_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(host_.c_str());
    if (address.sin_addr.s_addr == INADDR_NONE) {
        address.sin_addr.s_addr = INADDR_ANY;
    }
    address.sin_port = htons(static_cast<uint16_t>(port_));
    
    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        Logger::get_instance().log_error("Failed to bind socket to port " + std::to_string(port_) + ": " + strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    
    if (listen(server_fd_, BACKLOG) == -1) {
        Logger::get_instance().log_error("Failed to listen on socket: " + std::string(strerror(errno)));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    
    if (!epoll_->init()) {
        Logger::get_instance().log_error("Failed to initialize epoll instance");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    
    // Edge-triggered multiplexing on listening socket
    if (!epoll_->add_fd(server_fd_, EPOLLIN | EPOLLET)) {
        Logger::get_instance().log_error("Failed to register server_fd with epoll");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }
    
    running_.store(true);
    event_thread_ = std::make_unique<std::thread>(&Server::event_loop, this);
    
    Logger::get_instance().log_info("High-Performance Server started on " + host_ + ":" + 
                                    std::to_string(port_) + " with " + 
                                    std::to_string(thread_pool_->get_thread_count()) + 
                                    " worker threads (epoll edge-triggered)");
    return true;
}

void Server::stop() {
    if (running_.exchange(false)) {
        Logger::get_instance().log_info("Initiating graceful server shutdown...");
        
        // Wake up epoll event loop immediately using eventfd
        if (epoll_) {
            epoll_->wakeup();
        }
        
        // Join event loop thread first
        if (event_thread_ && event_thread_->joinable()) {
            event_thread_->join();
            event_thread_.reset();
        }
        
        // Shutdown thread pool
        if (thread_pool_) {
            thread_pool_->shutdown();
        }
        
        // Clean up connections with strict lock ordering
        std::vector<int> fds_to_close;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (const auto& [fd, conn] : connections_) {
                fds_to_close.push_back(fd);
            }
            connections_.clear();
        }
        
        for (int fd : fds_to_close) {
            epoll_->remove_fd(fd);
            close(fd);
        }
        
        if (server_fd_ != -1) {
            epoll_->remove_fd(server_fd_);
            close(server_fd_);
            server_fd_ = -1;
        }
        
        Logger::get_instance().log_info("Server shutdown completed cleanly.");
        Logger::get_instance().flush_logs();
    }
}

size_t Server::get_active_connections() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_.size();
}

void Server::event_loop() {
    std::vector<EpollWrapper::Event> events;
    auto last_cleanup_time = std::chrono::steady_clock::now();
    
    while (running_.load()) {
        int num_events = epoll_->wait_for_events(events, 500);
        
        if (num_events == -1) {
            if (errno != EINTR) {
                Logger::get_instance().log_error("epoll_wait error: " + std::string(strerror(errno)));
            }
            continue;
        }
        
        if (!running_.load()) {
            break;
        }
        
        for (int i = 0; i < num_events; ++i) {
            const auto& event = events[i];
            
            if (event.fd == server_fd_) {
                if (event.events & EPOLLIN) {
                    handle_accept();
                }
            } else {
                if (event.events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    close_connection(event.fd);
                } else if (event.events & EPOLLIN) {
                    handle_client_data(event.fd);
                } else if (event.events & EPOLLOUT) {
                    handle_client_write(event.fd);
                }
            }
        }
        
        // Periodically reap inactive connections (every 2 seconds)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup_time).count() >= 2) {
            cleanup_inactive_connections();
            last_cleanup_time = now;
        }
    }
}

void Server::handle_accept() {
    // Edge-triggered loop: exhaust all pending accept requests
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept4(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len, 
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // All incoming connections processed for this edge
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            Logger::get_instance().log_warn("Failed to accept connection: " + std::string(strerror(errno)));
            break;
        }
        
        // Connection limit protection
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            if (connections_.size() >= max_connections_) {
                // Reject connection gracefully with 503 Service Unavailable
                const char* rej = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                ssize_t s = send(client_fd, rej, strlen(rej), MSG_NOSIGNAL);
                (void)s;
                close(client_fd);
                continue;
            }
        }
        
        // Optimize TCP socket parameters
        EpollWrapper::set_socket_options(client_fd);
        
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
        std::string client_ip(ip_str);
        
        // Register client socket with Edge-Triggered + EPOLLONESHOT for thread-safe processing
        if (!epoll_->add_fd(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT)) {
            close(client_fd);
            continue;
        }
        
        auto connection = std::make_shared<Connection>(client_fd, client_ip);
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_[client_fd] = connection;
        }
    }
}

void Server::handle_client_data(int client_fd) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) {
            return;
        }
        conn = it->second;
    }
    
    if (!conn) return;
    
    char buffer[BUFFER_SIZE];
    bool eof_or_close = false;
    bool request_ready = false;
    
    {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        if (conn->closed) return;
        
        // In edge-triggered mode, read until EAGAIN / EWOULDBLOCK
        while (true) {
            ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes > 0) {
                if (conn->buffer.size() + bytes > MAX_REQUEST_SIZE) {
                    // Exceeded max request size limit: disconnect to protect server memory
                    eof_or_close = true;
                    break;
                }
                conn->buffer.append(buffer, bytes);
                conn->last_activity = std::chrono::steady_clock::now();
            } else if (bytes == 0) {
                // Client closed connection (FIN packet received)
                eof_or_close = true;
                break;
            } else {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Edge completely drained
                    break;
                }
                // Socket read error (e.g. ECONNRESET)
                eof_or_close = true;
                break;
            }
        }
        
        if (!eof_or_close) {
            if (!conn->processing_request && is_http_request_complete(conn->buffer)) {
                conn->processing_request = true;
                conn->request_start = std::chrono::steady_clock::now();
                request_ready = true;
            }
        }
    }
    
    if (eof_or_close) {
        close_connection(client_fd);
        return;
    }
    
    if (request_ready) {
        // Dispatch request processing to the worker thread pool
        // EPOLLONESHOT prevents other threads from concurrently reading from this fd
        thread_pool_->enqueue(&Server::handle_client_request, this, conn);
    } else {
        // Request still incomplete; re-arm socket for subsequent reads
        epoll_->modify_fd(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
    }
}

void Server::handle_client_request(std::shared_ptr<Connection> conn) {
    if (!conn) return;
    
    std::string raw_request;
    {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        if (conn->closed) return;
        raw_request = conn->buffer;
    }
    
    HttpRequest request = HttpRequest::parse(raw_request);
    total_requests_.fetch_add(1);
    
    HttpResponse response;
    
    if (!request.is_valid()) {
        response = HttpResponse::create_error_response(HttpStatus::BAD_REQUEST, "Malformed HTTP request");
        response.set_keep_alive(false);
    } else {
        bool keep_alive = request.is_keep_alive();
        {
            std::lock_guard<std::mutex> conn_lock(conn->mutex_);
            conn->keep_alive = keep_alive;
            conn->last_method = HttpRequest::method_to_string(request.get_method());
            conn->last_path = request.get_path();
            conn->user_agent = request.get_header("user-agent");
        }
        
        // Rate limiter check
        if (rate_limiter_ && rate_limiter_->is_enabled() && !rate_limiter_->is_allowed(conn->client_ip)) {
            response = HttpResponse::create_error_response(HttpStatus::SERVICE_UNAVAILABLE, "Rate limit exceeded");
            response.set_header("Retry-After", "1");
            response.set_keep_alive(false);
        } else if (request.get_method() == HttpMethod::GET || request.get_method() == HttpMethod::HEAD) {
            std::string path = request.get_path();
            if (path.find("/api/") == 0) {
                response = handle_api_request(request);
            } else {
                response = file_handler_->handle_file_request(path);
            }
            
            // For HEAD requests: headers match GET exactly, but body bytes are suppressed
            if (request.get_method() == HttpMethod::HEAD) {
                response.suppress_body(true);
            }
            response.set_keep_alive(conn->keep_alive);
        } else {
            response = HttpResponse::create_error_response(HttpStatus::METHOD_NOT_ALLOWED, "Method not supported");
            response.set_header("Allow", "GET, HEAD");
            response.set_keep_alive(conn->keep_alive);
        }
    }
    
    // Prepare serialized response buffer
    std::string response_data = response.to_string();
    
    {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        if (conn->closed) return;
        conn->pending_response = std::move(response_data);
        conn->response_offset = 0;
        conn->has_pending_write = true;
    }
    
    send_response_async(conn);
}

void Server::send_response_async(std::shared_ptr<Connection> conn) {
    if (!conn) return;
    
    bool close_needed = false;
    bool write_incomplete = false;
    
    {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        if (conn->closed || !conn->has_pending_write) return;
        
        while (conn->response_offset < conn->pending_response.size()) {
            size_t remaining = conn->pending_response.size() - conn->response_offset;
            ssize_t sent = send(conn->fd, conn->pending_response.c_str() + conn->response_offset, 
                                remaining, MSG_NOSIGNAL);
            
            if (sent > 0) {
                conn->response_offset += sent;
                conn->last_activity = std::chrono::steady_clock::now();
            } else if (sent == -1) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Socket send buffer full; arm EPOLLOUT to resume once writable
                    write_incomplete = true;
                    break;
                }
                // Broken pipe or reset by peer
                close_needed = true;
                break;
            } else {
                close_needed = true;
                break;
            }
        }
        
        if (!close_needed && !write_incomplete) {
            // All response bytes have been transmitted successfully
            conn->has_pending_write = false;
            conn->pending_response.clear();
            conn->response_offset = 0;
            conn->processing_request = false;
            
            if (conn->keep_alive) {
                conn->buffer.clear();
                conn->last_activity = std::chrono::steady_clock::now();
            } else {
                close_needed = true;
            }
        }
    }
    
    if (close_needed) {
        close_connection(conn->fd);
        return;
    }
    
    if (write_incomplete) {
        // Arm EPOLLOUT with Edge-Triggering and EPOLLONESHOT
        epoll_->modify_fd(conn->fd, EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
    } else {
        // Keep-alive connection: re-arm socket for subsequent HTTP requests
        epoll_->modify_fd(conn->fd, EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT);
    }
}

void Server::handle_client_write(int client_fd) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) return;
        conn = it->second;
    }
    
    send_response_async(conn);
}

void Server::close_connection(int client_fd) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) {
            return; // Already cleaned up
        }
        conn = it->second;
        connections_.erase(it);
    }
    
    if (conn) {
        std::lock_guard<std::mutex> conn_lock(conn->mutex_);
        conn->closed = true;
        conn->has_pending_write = false;
    }
    
    epoll_->remove_fd(client_fd);
    close(client_fd);
}

void Server::cleanup_inactive_connections() {
    auto now = std::chrono::steady_clock::now();
    std::vector<int> inactive_fds;
    
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto& [fd, conn] : connections_) {
            std::lock_guard<std::mutex> conn_lock(conn->mutex_);
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - conn->last_activity);
            if (elapsed.count() > CONNECTION_TIMEOUT_SECONDS && !conn->has_pending_write && !conn->processing_request) {
                inactive_fds.push_back(fd);
            }
        }
    }
    
    for (int fd : inactive_fds) {
        close_connection(fd);
    }
}

HttpResponse Server::handle_api_request(const HttpRequest& request) {
    std::string path = request.get_path();
    
    if (path == "/api/info" || path == "/api/status") {
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        
        size_t cache_hits = 0, cache_misses = 0, cache_entries = 0, cache_memory = 0;
        file_handler_->get_cache_stats(cache_hits, cache_misses, cache_entries, cache_memory);
        
        std::ostringstream body;
        body << "{\n";
        body << "  \"server\": \"MultithreadedWebServer/1.0\",\n";
        body << "  \"engine\": \"C++17 Epoll Edge-Triggered Reactor\",\n";
        body << "  \"timestamp\": \"" << std::put_time(std::gmtime(&now_c), "%Y-%m-%dT%H:%M:%SZ") << "\",\n";
        body << "  \"thread_pool_size\": " << thread_pool_->get_thread_count() << ",\n";
        body << "  \"queue_size\": " << thread_pool_->get_queue_size() << ",\n";
        body << "  \"active_tasks\": " << thread_pool_->get_active_tasks() << ",\n";
        body << "  \"active_connections\": " << get_active_connections() << ",\n";
        body << "  \"max_connections\": " << max_connections_ << ",\n";
        body << "  \"total_requests\": " << total_requests_.load() << ",\n";
        body << "  \"document_root\": \"" << file_handler_->get_document_root() << "\",\n";
        body << "  \"cache\": {\n";
        body << "    \"hits\": " << cache_hits << ",\n";
        body << "    \"misses\": " << cache_misses << ",\n";
        body << "    \"entries\": " << cache_entries << ",\n";
        body << "    \"memory_usage_bytes\": " << cache_memory;
        if (cache_hits + cache_misses > 0) {
            double hit_ratio = static_cast<double>(cache_hits) / (cache_hits + cache_misses) * 100.0;
            body << ",\n    \"hit_ratio_percent\": " << std::fixed << std::setprecision(1) << hit_ratio;
        }
        body << "\n  }\n";
        body << "}\n";
        
        HttpResponse response(HttpStatus::OK);
        response.set_body(body.str());
        response.set_content_type("application/json");
        return response;
    }
    
    return HttpResponse::create_error_response(HttpStatus::NOT_FOUND, "API endpoint not found");
}

bool Server::is_http_request_complete(const std::string& buffer) {
    size_t header_end = buffer.find("\r\n\r\n");
    size_t delim_len = 4;
    if (header_end == std::string::npos) {
        header_end = buffer.find("\n\n");
        delim_len = 2;
        if (header_end == std::string::npos) {
            return false;
        }
    }
    
    std::string headers = buffer.substr(0, header_end);
    std::istringstream header_stream(headers);
    std::string line;
    size_t content_length = 0;
    bool has_content_length = false;
    
    std::getline(header_stream, line); // Request line
    
    while (std::getline(header_stream, line) && !line.empty()) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string name = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            
            name.erase(name.find_last_not_of(" \t") + 1);
            name.erase(0, name.find_first_not_of(" \t"));
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            
            if (name == "content-length") {
                try {
                    content_length = std::stoull(value);
                    has_content_length = true;
                } catch (const std::exception&) {
                    content_length = 0;
                }
                break;
            }
        }
    }
    
    size_t expected_size = header_end + delim_len;
    if (has_content_length) {
        expected_size += content_length;
    }
    
    return buffer.size() >= expected_size;
}

bool Server::is_likely_http_request(const std::string& buffer) {
    if (buffer.empty()) return false;
    
    size_t first_line_end = buffer.find('\n');
    if (first_line_end == std::string::npos && buffer.size() < 16) {
        return false;
    }
    
    std::string first_line = (first_line_end != std::string::npos) 
        ? buffer.substr(0, first_line_end) 
        : buffer;
    
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.pop_back();
    }
    
    static const std::vector<std::string> methods = {"GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS"};
    for (const auto& method : methods) {
        if (first_line.compare(0, method.length(), method) == 0 && 
            first_line.size() > method.length() && 
            first_line[method.length()] == ' ') {
            return true;
        }
    }
    
    return false;
}
