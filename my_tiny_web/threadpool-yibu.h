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
    ThreadPool(int min = 4, int max = thread::hardware_concurrency());
    ~ThreadPool();
    void addTask(function<void()> f);

private:
    void manager();
    void worker();
private:
    thread* m_manager;
    map<thread::id, thread> m_workers; 
    vector<thread::id> m_ids; 
    int m_minThreads;
    int m_maxThreads; 
    atomic<bool> m_stop;    //表示线程池是否被停止
    atomic<int> m_curThreads;   //表示当前线程的数量
    atomic<int> m_idleThreads;  //表示当前空闲线程的数量
    atomic<int> m_exitNumber; //用于在线程池中以线程安全的方式存储和操作退出标志或计数器
    queue<function<void()>> m_tasks;
    mutex m_idsMutex;   //管理线程ID列表的锁
    mutex m_queueMutex; //队列操作的锁
    condition_variable m_condition;
};