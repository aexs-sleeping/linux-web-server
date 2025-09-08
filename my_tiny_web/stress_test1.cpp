#include "threadpool-dynamic.h"
#include <unistd.h>
#include <cstdio>
#include <vector>
#include <chrono>

int main() {
    // 创建线程池，初始2个线程，最大8个线程
    ThreadPool pool(2, 8);

    // 记录任务开始时间
    auto start = std::chrono::steady_clock::now();

    // 一次性提交20个耗时任务
    int task_count = 20;
    for (int i = 0; i < task_count; ++i) {
        pool.addTask([i]{
            printf("任务 %d 开始执行，线程ID: %zu\n", i, std::hash<std::thread::id>{}(std::this_thread::get_id()));
            fflush(stdout);
            sleep(3); // 每个任务模拟耗时
            printf("任务 %d 执行完毕，线程ID: %zu\n", i, std::hash<std::thread::id>{}(std::this_thread::get_id()));
            fflush(stdout);
        });
    }

    // 主线程等待所有任务完成
    // 这里简单 sleep 足够时间，实际可用条件变量或 future 等更优雅方式
    sleep(35);

    auto end = std::chrono::steady_clock::now();
    printf("所有任务预计已完成，总耗时约 %ld 秒\n", std::chrono::duration_cast<std::chrono::seconds>(end - start).count());
    fflush(stdout);

    // 程序退出时自动析构线程池
    return 0;
}