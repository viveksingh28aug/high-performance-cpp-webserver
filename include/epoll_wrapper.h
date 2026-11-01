#pragma once

#include <sys/epoll.h>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>

class EpollWrapper {
public:
    struct Event {
        int fd;
        uint32_t events;
        void* data;
    };
    
    using EventHandler = std::function<void(const Event&)>;
    
    EpollWrapper();
    ~EpollWrapper();
    
    // Prevent copying
    EpollWrapper(const EpollWrapper&) = delete;
    EpollWrapper& operator=(const EpollWrapper&) = delete;
    
    // Allow moving
    EpollWrapper(EpollWrapper&& other) noexcept;
    EpollWrapper& operator=(EpollWrapper&& other) noexcept;
    
    bool init();
    bool add_fd(int fd, uint32_t events, void* data = nullptr);
    bool modify_fd(int fd, uint32_t events, void* data = nullptr);
    bool remove_fd(int fd);
    
    // Non-blocking wakeup using eventfd for instant shutdown
    void wakeup();
    
    int wait_for_events(std::vector<Event>& events, int timeout_ms = -1);
    void set_event_handler(EventHandler handler) { event_handler_ = std::move(handler); }
    
    static bool set_non_blocking(int fd);
    static bool set_socket_options(int fd);
    
    int get_epoll_fd() const { return epoll_fd_; }
    bool is_valid() const { return epoll_fd_ != -1; }
    
private:
    int epoll_fd_;
    int wakeup_fd_;
    std::vector<epoll_event> events_buffer_;
    EventHandler event_handler_;
    std::unordered_map<int, void*> fd_data_;
    mutable std::mutex fd_data_mutex_;
    
    static constexpr int MAX_EVENTS = 1024;
};
