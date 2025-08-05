### DispatchQueue
[中文](README.zh.md)

This project is a C++20 implementation of `DispatchQueue`, inspired by Apple's Grand Central Dispatch (GCD) architecture. It provides the following components:

- `DispatchSerialQueue`
- `DispatchConcurrentQueue`
- `DispatchGroup`
- `DispatchWorkItem`

All APIs are designed with reference to the definitions on [Apple Developer](https://developer.apple.com/documentation/dispatch).

---

### Usage

Place the `DispatchQueue` folder into your project directory and add the following configuration to your `CMakeLists.txt`:

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

Possible output:

```bash
current thread: 0x1fac45f00
async task thread: 0x16d437000
sync task thread: 0x1fac45f00
```

---

### Implementation Details

All types of DispatchQueues (both serial and concurrent) are scheduled by a unified thread pool (Executor). When a task is submitted, it is passed to the Executor. The Executor dispatches tasks based on the DispatchQueue's priority using an FCFS (First-Come, First-Served) strategy. The thread pool design is inspired by [folly::CPUThreadPoolExecutor](https://github.com/facebook/folly/blob/main/folly/executors/CPUThreadPoolExecutor.h).

---

### Todo

- [x] Replace the underlying bounded queue with a dynamic version.
- [ ] Conduct performance testing for DispatchQueue.
- [x] Support tasks with return values and a notify callback mechanism.
- [ ] Support the `Barrier` flag for `DispatchWorkItem`.

