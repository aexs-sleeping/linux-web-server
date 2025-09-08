#pragma once
#include <thread>
#include <mutex>
#include <vector>
#include <queue>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <map>
#include <future>
using namespace std;

// 线程池类
class ThreadPool
{
public:
    /// @brief ThreadPool::ThreadPool(int min, int max) 是 ThreadPool 类的构造函数，用于初始化线程池。
    /// @param min 当前线程数初始化为 min，并输出线程数量信息。
    /// @param max 最大线程数设置为 max。构造函数还创建了一个管理线程，并根据 min 值创建相应数量的工作线程，工作线程会执行 worker 函数。
    ThreadPool(int min = 4, int max = thread::hardware_concurrency());
    ~ThreadPool();
    void addTask(function<void()> f);

private:
    void manager();
    void worker();
private:
    thread* m_manager;
    map<thread::id, thread> m_workers; 
    vector<thread::id> m_ids; //存储将要退出的线程ID
    int m_minThreads;   //表示线程池中允许的最小线程数量
    int m_maxThreads;   //表示线程池中允许的最大线程数量
    atomic<bool> m_stop;    //表示线程池是否被停止
    atomic<int> m_curThreads;   //表示当前线程的数量
    atomic<int> m_idleThreads;  //表示当前空闲线程的数量
    atomic<int> m_exitNumber; //用于在线程池中以线程安全的方式存储和操作退出标志或计数器
    queue<function<void()>> m_tasks;
    mutex m_idsMutex;   //管理线程ID列表的锁
    mutex m_queueMutex; //队列操作的锁
    condition_variable m_condition; //用于实现线程间的同步机制，通常用于线程池中协调任务的等待和通知操作
};