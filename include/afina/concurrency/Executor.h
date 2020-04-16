#ifndef AFINA_CONCURRENCY_EXECUTOR_H
#define AFINA_CONCURRENCY_EXECUTOR_H

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <chrono>
#include <iostream>

namespace Afina {
namespace Concurrency {

/**
 * # Thread pool
 */
class Executor {
public:
    enum class State {
        // Threadpool is fully operational, tasks could be added and get executed
        kRun,

        // Threadpool is on the way to be shutdown, no ned task could be added, but existing will be
        // completed as requested
        kStopping,

        // Threadppol is stopped
        kStopped
    };

    Executor(std::string name, int size, int _low_watermark, int _hight_watermark,
        int _max_queue_size, int _idle_time):
    low_watermark(_low_watermark), hight_watermark(_hight_watermark),
    max_queue_size(_max_queue_size), idle_time(_idle_time) {}
    ~Executor() {Stop(true);}

    /**
     * Signal thread pool to stop, it will stop accepting new jobs and close threads just after each become
     * free. All enqueued jobs will be complete.
     *
     * In case if await flag is true, call won't return until all background jobs are done and all threads are stopped
     */
    void Stop(bool await = false) {
        state = State::kStopping;
        std::unique_lock<std::mutex> lock(mutex);
        while (!tasks.empty()) {
            to_stop.wait(lock);
        }
        empty_condition.notify_all(); // wake up and return
        while (await && threads_count) {
            to_stop.wait(lock);   
        }
        state = State::kStopped;
    }

    /**
     * Add function to be executed on the threadpool. Method returns true in case if task has been placed
     * onto execution queue, i.e scheduled for execution and false otherwise.
     *
     * That function doesn't wait for function result. Function could always be written in a way to notify caller about
     * execution finished by itself
     */
    template <typename F, typename... Types> bool Execute(F &&func, Types... args) {
        // Prepare "task"
        auto exec = std::bind(std::forward<F>(func), std::forward<Types>(args)...);

        std::unique_lock<std::mutex> lock(this->mutex);
        if (state != State::kRun) {
            return false;
        }

        // Enqueue new task
        if (tasks.size() < max_queue_size) {
            tasks.push_back(exec);
            if (waiting_count) {
                empty_condition.notify_one();
            }
            else if (threads_count < hight_watermark) {
                std::thread th(&Executor::perform, this);
                //threads.push_back(th);
                threads_count++;
                th.detach();
            }
            return true;
        }
        return false;
    }

private:
    // No copy/move/assign allowed
    Executor(const Executor &);            // = delete;
    Executor(Executor &&);                 // = delete;
    Executor &operator=(const Executor &); // = delete;
    Executor &operator=(Executor &&);      // = delete;

    /**
     * Main function that all pool threads are running. It polls internal task queue and execute tasks
     */
    //friend void perform(Executor *executor) {
    void perform() {
        bool waiting = false;
        while (state != State::kStopped) {
            mutex.lock(); // mutex, а не unique_lock, чтобы не держать в task
            std::cout << threads_count << " потоков работает, " << tasks.size() << "задач в очереди" << std::endl;
            if (!tasks.empty()) {
                auto task = std::move(tasks.front());
                tasks.pop_front();
                if (tasks.empty() && state != State::kRun) {
                    to_stop.notify_one();
                }
                mutex.unlock();
                task();
            }
            else {
                mutex.unlock();
                std::unique_lock<std::mutex> lock(mutex);
                waiting_count++;
                while(tasks.empty()) {
                    if (waiting && threads_count > low_watermark or state != State::kRun) {
                        waiting_count--;
                        threads_count--;
                        if (threads_count == 0) {
                            to_stop.notify_one();
                        }
                        return;
                    } 
                    empty_condition.wait_for(lock, std::chrono::milliseconds(idle_time));
                    waiting = true;
                }
                waiting = false;
                waiting_count--;
            }
        }
        std::unique_lock<std::mutex> lock(mutex);
        threads_count--;
        if (threads_count == 0) {
            to_stop.notify_one();
        }
    }

    /**
     * Mutex to protect state below from concurrent modification
     */
    std::mutex mutex;

    /**
     * Conditional variable to await new data in case of empty queue
     */
    std::condition_variable empty_condition;
    std::condition_variable to_stop;

    /**
     * Vector of actual threads that perorm execution
     */
    //std::vector<std::thread> threads;
    int threads_count = 0;

    /**
     * Task queue
     */
    std::deque<std::function<void()>> tasks;

    /**
     * Flag to stop bg threads
     */
    State state;

    int waiting_count = 0;
    int low_watermark;
    int hight_watermark;
    int max_queue_size;
    int idle_time;
};

} // namespace Concurrency
} // namespace Afina

#endif // AFINA_CONCURRENCY_EXECUTOR_H
