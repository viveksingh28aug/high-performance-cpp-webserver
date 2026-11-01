#include "epoll_wrapper.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <iostream>
#include <cstring>
#include <cerrno>

EpollWrapper::EpollWrapper() : epoll_fd_(-1), wakeup_fd_(-1) {
    events_buffer_.resize(MAX_EVENTS);
}

EpollWrapper::~EpollWrapper() {
    if (wakeup_fd_ != -1) {
        if (epoll_fd_ != -1) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, wakeup_fd_, nullptr);
        }
        close(wakeup_fd_);
        wakeup_fd_ = -1;
    }
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

EpollWrapper::EpollWrapper(EpollWrapper&& other) noexcept
    : epoll_fd_(other.epoll_fd_),
      wakeup_fd_(other.wakeup_fd_),
      events_buffer_(std::move(other.events_buffer_)),
      event_handler_(std::move(other.event_handler_)),
      fd_data_(std::move(other.fd_data_)) {
    other.epoll_fd_ = -1;
    other.wakeup_fd_ = -1;
}

EpollWrapper& EpollWrapper::operator=(EpollWrapper&& other) noexcept {
    if (this != &other) {
        if (wakeup_fd_ != -1) {
            close(wakeup_fd_);
        }
        if (epoll_fd_ != -1) {
            close(epoll_fd_);
        }
        epoll_fd_ = other.epoll_fd_;
        wakeup_fd_ = other.wakeup_fd_;
        events_buffer_ = std::move(other.events_buffer_);
        event_handler_ = std::move(other.event_handler_);
        fd_data_ = std::move(other.fd_data_);
        other.epoll_fd_ = -1;
        other.wakeup_fd_ = -1;
    }
    return *this;
}

bool EpollWrapper::init() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        std::cerr << "Failed to create epoll instance: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Create an eventfd for immediate asynchronous wakeup (e.g. graceful shutdown)
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ == -1) {
        std::cerr << "Failed to create eventfd for epoll wakeup: " << strerror(errno) << std::endl;
        close(epoll_fd_);
        epoll_fd_ = -1;
        return false;
    }
    
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = wakeup_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) == -1) {
        std::cerr << "Failed to register wakeup_fd with epoll: " << strerror(errno) << std::endl;
        close(wakeup_fd_);
        close(epoll_fd_);
        wakeup_fd_ = -1;
        epoll_fd_ = -1;
        return false;
    }
    
    return true;
}

void EpollWrapper::wakeup() {
    if (wakeup_fd_ != -1) {
        uint64_t val = 1;
        ssize_t s = write(wakeup_fd_, &val, sizeof(val));
        (void)s;
    }
}

bool EpollWrapper::add_fd(int fd, uint32_t events, void* data) {
    if (epoll_fd_ == -1) return false;
    
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) == -1) {
        if (errno != EEXIST) {
            std::cerr << "Failed to add fd " << fd << " to epoll: " << strerror(errno) << std::endl;
        }
        return false;
    }
    
    if (data) {
        std::lock_guard<std::mutex> lock(fd_data_mutex_);
        fd_data_[fd] = data;
    }
    
    return true;
}

bool EpollWrapper::modify_fd(int fd, uint32_t events, void* data) {
    if (epoll_fd_ == -1) return false;
    
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        if (errno != ENOENT && errno != EBADF) {
            std::cerr << "Failed to modify fd " << fd << " in epoll: " << strerror(errno) << std::endl;
        }
        return false;
    }
    
    if (data) {
        std::lock_guard<std::mutex> lock(fd_data_mutex_);
        fd_data_[fd] = data;
    }
    
    return true;
}

bool EpollWrapper::remove_fd(int fd) {
    if (epoll_fd_ == -1 || fd < 0) return false;
    
    bool epoll_success = true;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        if (errno != EBADF && errno != ENOENT) {
            std::cerr << "Failed to remove fd " << fd << " from epoll: " << strerror(errno) << std::endl;
        }
        epoll_success = false;
    }
    
    {
        std::lock_guard<std::mutex> lock(fd_data_mutex_);
        fd_data_.erase(fd);
    }
    
    return epoll_success;
}

int EpollWrapper::wait_for_events(std::vector<Event>& events, int timeout_ms) {
    if (epoll_fd_ == -1) return -1;
    
    int num_events = epoll_wait(epoll_fd_, events_buffer_.data(), MAX_EVENTS, timeout_ms);
    
    if (num_events == -1) {
        if (errno != EINTR) {
            std::cerr << "epoll_wait failed: " << strerror(errno) << std::endl;
        }
        return -1;
    }
    
    events.clear();
    events.reserve(num_events);
    
    std::lock_guard<std::mutex> lock(fd_data_mutex_);
    
    for (int i = 0; i < num_events; ++i) {
        int event_fd = events_buffer_[i].data.fd;
        
        // Handle wakeup notification from eventfd
        if (event_fd == wakeup_fd_) {
            uint64_t val = 0;
            while (read(wakeup_fd_, &val, sizeof(val)) > 0) {}
            continue;
        }
        
        Event event{};
        event.fd = event_fd;
        event.events = events_buffer_[i].events;
        
        auto it = fd_data_.find(event_fd);
        event.data = (it != fd_data_.end()) ? it->second : nullptr;
        
        events.push_back(event);
    }
    
    return static_cast<int>(events.size());
}

bool EpollWrapper::set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        std::cerr << "Failed to get file flags for fd " << fd << ": " << strerror(errno) << std::endl;
        return false;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        std::cerr << "Failed to set non-blocking for fd " << fd << ": " << strerror(errno) << std::endl;
        return false;
    }
    
    return true;
}

bool EpollWrapper::set_socket_options(int fd) {
    int opt = 1;
    // Disable Nagle's algorithm for low-latency HTTP request-response cycles
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1) {
        std::cerr << "Warning: Could not set TCP_NODELAY on fd " << fd << ": " << strerror(errno) << std::endl;
    }
    
    // Set socket receive/send buffers
    int buffer_size = 256 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    
    return true;
}
