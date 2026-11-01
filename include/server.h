#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include "epoll_wrapper.h"
#include "thread_pool.h"
#include "http_request.h"
#include "http_response.h"
#include "file_handler.h"
#include "rate_limiter.h"
#include "logger.h"

enum class ConnectionState {
    READING,
    PROCESSING,
    WRITING,
    CLOSED
};

struct Connection {
    int fd;
    std::string client_ip;
    std::string buffer;
    bool keep_alive;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point request_start;
    mutable std::mutex mutex_;
    
    // Partial write state
    std::string pending_response;
    size_t response_offset;
    bool has_pending_write;
    bool processing_request;
    bool closed;
    
    // Last request info for access logging
    std::string last_method;
    std::string last_path;
    std::string user_agent;
    
    Connection(int socket_fd, std::string ip = "") 
        : fd(socket_fd), client_ip(std::move(ip)), keep_alive(false), 
          last_activity(std::chrono::steady_clock::now()),
          request_start(std::chrono::steady_clock::now()),
          response_offset(0), has_pending_write(false), 
          processing_request(false), closed(false) {}
};

class Server {
public:
    explicit Server(int port = 8080, const std::string& host = "0.0.0.0", size_t thread_count = 0);
    ~Server();
    
    // Prevent copying
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    
    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    
    int get_port() const { return port_; }
    size_t get_active_connections() const;
    size_t get_total_requests() const { return total_requests_.load(); }
    void set_max_connections(size_t max_conns) { max_connections_ = max_conns; }
    size_t get_max_connections() const { return max_connections_; }
    ThreadPool* get_thread_pool() const { return thread_pool_.get(); }
    
private:
    void event_loop();
    void handle_accept();
    void handle_client_data(int client_fd);
    void handle_client_request(std::shared_ptr<Connection> conn);
    void handle_client_write(int client_fd);
    void send_response_async(std::shared_ptr<Connection> conn);
    void close_connection(int client_fd);
    void cleanup_inactive_connections();
    HttpResponse handle_api_request(const HttpRequest& request);
    std::string get_client_ip(int client_fd);
    bool is_http_request_complete(const std::string& buffer);
    bool is_likely_http_request(const std::string& buffer);
    
    int server_fd_;
    int port_;
    std::string host_;
    std::atomic<bool> running_;
    std::atomic<size_t> total_requests_{0};
    
    std::unique_ptr<EpollWrapper> epoll_;
    std::unique_ptr<ThreadPool> thread_pool_;
    std::unique_ptr<std::thread> event_thread_;
    std::unique_ptr<FileHandler> file_handler_;
    std::unique_ptr<RateLimiter> rate_limiter_;
    
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    mutable std::mutex connections_mutex_;
    
    static constexpr int BUFFER_SIZE = 8192;
    static constexpr int BACKLOG = 4096;
    static constexpr int CONNECTION_TIMEOUT_SECONDS = 30;
    static constexpr size_t MAX_REQUEST_SIZE = 64 * 1024;
    
    size_t max_connections_;
};
