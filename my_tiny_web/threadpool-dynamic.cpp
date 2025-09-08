#include "threadpool-dynamic.h"
#include <cstdio>
#include <functional>

ThreadPool::ThreadPool(int min, int max) : m_maxThreads(max),
m_minThreads(min), m_stop(false), m_exitNumber(0)
{
    //m_idleThreads = m_curThreads = max / 2;
    m_idleThreads = m_curThreads = min;
    printf("线程数量: %d\n", m_curThreads.load());
    m_manager = new thread(&ThreadPool::manager, this);
    for (int i = 0; i < m_curThreads; ++i)
    {
        thread t(&ThreadPool::worker, this);
        m_workers.insert(make_pair(t.get_id(), move(t)));
    }
}

ThreadPool::~ThreadPool()
{
    m_stop = true;
    m_condition.notify_all();
    for (auto& it : m_workers)
    {
        thread& t = it.second;
        if (t.joinable())
        {
            printf("******** 线程 %zu 将要退出了...\n", std::hash<std::thread::id>{}(t.get_id()));
            t.join();
        }
    }
    if (m_manager->joinable())
    {
        m_manager->join();
    }
    delete m_manager;
}

void ThreadPool::addTask(function<void()> f)
{
    {
        lock_guard<mutex> locker(m_queueMutex);
        m_tasks.emplace(f);
    }
    m_condition.notify_one();
}

/// @brief ThreadPool::manager 是一个管理线程池的函数，它通过循环监控线程池的状态，根据空闲线程数量和当前线程数量动态调整线程池的大小。当空闲线程过多时会销毁部分线程，而当没有空闲线程且线程数量未达到上限时会创建新线程
void ThreadPool::manager()
{
    while (!m_stop.load())
    {
        this_thread::sleep_for(chrono::seconds(2));
        int idle = m_idleThreads.load(); //空闲线程数
        int current = m_curThreads.load();//当前线程数
        printf("manager check: idle=%d current=%d min=%d\n", idle, current, m_minThreads);
        if (idle > current / 2 && current > m_minThreads)
        {
            m_exitNumber.store(2);
            m_condition.notify_all();
            unique_lock<mutex> lck(m_idsMutex);
            for (const auto& id : m_ids)
            {
                auto it = m_workers.find(id);
                if (it != m_workers.end())
                {
                    printf("############## 线程 %zu 被销毁了....\n", std::hash<std::thread::id>{}((*it).first));
                    (*it).second.join();
                    m_workers.erase(it);
                }
            }
            m_ids.clear();
        }
        else if (idle == 0 && current < m_maxThreads)
        {
            thread t(&ThreadPool::worker, this);
            printf("+++++++++++++++ 添加了一个线程, id: %zu\n", std::hash<std::thread::id>{}(t.get_id()));
            m_workers.insert(make_pair(t.get_id(), move(t)));
            m_curThreads++;
            m_idleThreads++;
        }
    }
}
/// @brief ThreadPool::worker 是线程池中的工作线程函数，它在一个循环中持续运行，等待任务队列中的任务。当任务队列为空时，线程会进入等待状态；当有任务时，线程取出任务执行，并在特定条件下退出线程并更新线程池状态。
void ThreadPool::worker()
{
    while (!m_stop.load())
    {
        function<void()> task = nullptr;
        {
            unique_lock<mutex> locker(m_queueMutex);
            while (!m_stop && m_tasks.empty())
            {
                m_condition.wait(locker);
                if (m_exitNumber.load() > 0)//当设置为大于0的时候 说明一些线程需要退出了
                {
                    printf("----------------- 线程任务结束, ID: %zu\n", std::hash<std::thread::id>{}(this_thread::get_id()));
                    m_exitNumber--;
                    m_curThreads--;
                    unique_lock<mutex> lck(m_idsMutex);
                    m_ids.emplace_back(this_thread::get_id());
                    return;
                }
            }

            if (!m_tasks.empty())
            {
                printf("取出一个任务...\n");
                task = move(m_tasks.front());
                m_tasks.pop();
            }
        }

        if (task)
        {
            m_idleThreads--;
            task();
            m_idleThreads++;
        }
        printf("worker idleThreads=%d\n", m_idleThreads.load());
    }
}

