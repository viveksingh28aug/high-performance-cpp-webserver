#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <type_traits>
#include <memory>
#include <stdexcept>

class ThreadPool {
public:
    using Task = std::function<void()>;
    
    explicit ThreadPool(size_t thread_count = std::thread::hardware_concurrency());
    ~ThreadPool();
    
    // Prevent copying
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>;
    
    void shutdown();
    void wait_until_idle();
    
    size_t get_queue_size() const;
    size_t get_thread_count() const { return workers_.size(); }
    size_t get_active_tasks() const { return active_tasks_.load(); }
    bool is_shutdown() const { return shutdown_.load(); }
    
private:
    void worker_thread();
    
    std::vector<std::thread> workers_;
    std::queue<Task> tasks_;
    
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::condition_variable idle_condition_;
    
    std::atomic<bool> shutdown_;
    std::atomic<size_t> active_tasks_;
};

template<typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) 
    -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    
    using return_type = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;
    
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    std::future<return_type> result = task->get_future();
    
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (shutdown_.load()) {
            throw std::runtime_error("ThreadPool is shutdown, cannot enqueue task");
        }
        tasks_.emplace([task]() { (*task)(); });
    }
    
    condition_.notify_one();
    return result;
}
