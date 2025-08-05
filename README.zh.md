### DispatchQueue

本项目是一个使用 C++20 标准库实现的 `DispatchQueue`，参考了 Apple 的 Grand Central Dispatch (GCD) 架构设计。提供以下组件：

- `DispatchSerialQueue`
- `DispatchConcurrentQueue`
- `DispatchGroup`
- `DispatchWorkItem`

所有 API 的设计均参考 [Apple Developer](https://developer.apple.com/documentation/dispatch) 上的定义。

---

### Usage | 使用方法

将 `DispatchQueue` 文件夹放入你的项目目录，并在 `CMakeLists.txt` 中添加如下配置：

```cmake
cmake_minimum_required(VERSION 3.30)
project(LearnAndTry)
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(DispatchQueue)

add_executable(LearnAndTry main.cpp)
target_link_libraries(LearnAndTry PRIVATE DispatchQueue)
```

main.cpp:

```c++
#include <iostream>
#include <thread>
#include <DispatchQueue/DispatchSerialQueue.h>

void asyncTask() {
    std::cout << "async task thread: " << std::this_thread::get_id() << '\n';
}

int main() {
    std::cout << "current thread: " << std::this_thread::get_id() << '\n';

    auto queue = DispatchSerialQueue("queue");
    queue.async(asyncTask);
    queue.sync([] {
        std::cout << "sync task thread: " << std::this_thread::get_id() << '\n';
    });
}
```

possible output:

```bash
current thread: 0x1fac45f00
async task thread: 0x16d437000
sync task thread: 0x1fac45f00
```

---

### 实现说明

所有类型的 DispatchQueue（包括串行和并发）统一由线程池（Executor）调度。任务提交后，会传递给 Executor。Executor 根据 DispatchQueue 的优先级，以 FCFS 策略分发执行。线程池设计参考自 [folly::CPUThreadPoolExecutor](https://github.com/facebook/folly/blob/main/folly/executors/CPUThreadPoolExecutor.h)。

---

### Todo

- [x] 将底层容器由有界 MPMC 队列替换为可扩容队列
- [ ] 对 DispatchQueue 进行性能测试
- [x] 支持带返回值的任务 和 notify 回调机制
- [ ] 支持 DispatchWorkItem 的 flag: Barrier 特性
