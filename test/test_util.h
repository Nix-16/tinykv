#pragma once

// 极简测试断言宏：失败即打印位置并 abort。
// 保持零依赖，便于在 CTest 下直接运行。

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "CHECK failed: %s\n  at %s:%d\n", #cond, \
                         __FILE__, __LINE__);                             \
            std::abort();                                                 \
        }                                                                 \
    } while (0)
