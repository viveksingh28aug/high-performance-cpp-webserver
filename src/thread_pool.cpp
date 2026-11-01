#include "thread_pool.h"
#include <iostream>

ThreadPool::ThreadPool(size_t thread_count) 
    : shutdown_(false), active_tasks_(0) {
    if (thread_count == 0) {
        thread_count = std::thread::hardware_concurrency();
        if (thread_count == 0) {
            thread_count = 4; // safe fallback
        }
    }
    
    workers_.reserve(thread_count);
    
    for (size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back(&ThreadPool::worker_thread, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    bool expected = false;
    if (shutdown_.compare_exchange_strong(expected, true)) {
        condition_.notify_all();
        
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        
        workers_.clear();
    }
}

void ThreadPool::wait_until_idle() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    idle_condition_.wait(lock, [this] {
        return tasks_.empty() && active_tasks_.load() == 0;
    });
}

size_t ThreadPool::get_queue_size() const {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

void ThreadPool::worker_thread() {
    while (true) {
        Task task;
        
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this] {
                return shutdown_.load() || !tasks_.empty();
            });
            
            if (shutdown_.load() && tasks_.empty()) {
                break;
            }
            
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
                active_tasks_.fetch_add(1);
            }
        }
        
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "ThreadPool worker caught exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "ThreadPool worker caught unknown exception" << std::endl;
            }
            
            active_tasks_.fetch_sub(1);
            
            // Notify if queue is empty and all tasks completed
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (tasks_.empty() && active_tasks_.load() == 0) {
                    idle_condition_.notify_all();
                }
            }
        }
    }
}
