#include <vector>
#include <random>
#include <cstdint>
#include <iostream>
#include <algorithm>
#include "refalloc.h"

struct Allocation
{
    void* ptr;
    size_t size;
    uint32_t pattern;
};

static void fill_memory(void* ptr, size_t size, uint32_t pattern)
{
    uint32_t* p = (uint32_t*)ptr;
    size_t count = size / sizeof(uint32_t);

    for (size_t i = 0; i < count; i++)
        p[i] = pattern;

    // handle tail bytes
    uint8_t* b = (uint8_t*)ptr;
    for (size_t i = count * 4; i < size; i++)
        b[i] = (uint8_t)pattern;
}

static void verify_memory(void* ptr, size_t size, uint32_t pattern)
{
    uint32_t* p = (uint32_t*)ptr;
    size_t count = size / sizeof(uint32_t);

    for (size_t i = 0; i < count; i++)
    {
        if (p[i] != pattern)
        {
            std::cout << "CORRUPTION detected at word " << i << "\n";
            std::abort();
        }
    }

    uint8_t* b = (uint8_t*)ptr;
    for (size_t i = count * 4; i < size; i++)
    {
        if (b[i] != (uint8_t)pattern)
        {
            std::cout << "CORRUPTION detected in tail\n";
            std::abort();
        }
    }
}

int main()
{
    std::vector<Allocation> live;
    size_t liveSize = 0;

    std::random_device rd;
    std::mt19937 rng(rd());

    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> size_dist(1, 6000);
    std::uniform_int_distribution<int> large_dist(1, 200000);
    std::uniform_int_distribution<uint32_t> pattern_dist(0, 0xFFFFFFFF);

    const int ITERATIONS = 1'000'000;

    for (int i = 0; i < ITERATIONS; i++)
    {
        int op = op_dist(rng);

        bool do_alloc =
            live.size() < 1000 || op < 70; // bias toward allocation

        if (do_alloc)
        {
            size_t size;

            // mix small / medium / large allocations
            int type = op_dist(rng);

            if (type < 95)
                size = size_dist(rng);          // small/medium
            else
                size = large_dist(rng);         // large stress

            void* ptr = rnew(size);
            if (!ptr)
            {
                std::cout << "OUT OF MEMORY\n";
                break;
            }

            uint32_t pattern = pattern_dist(rng);

            fill_memory(ptr, size, pattern);

            live.push_back({ ptr, size, pattern });
            liveSize += size;
        }
        else
        {
            if (live.empty())
                continue;

            std::uniform_int_distribution<size_t> pick(0, live.size() - 1);
            size_t idx = pick(rng);

            Allocation a = live[idx];

            verify_memory(a.ptr, a.size, a.pattern);

            rfree(a.ptr);            

            live[idx] = live.back();
            live.pop_back();
            liveSize -= a.size;
        }

        // occasional progress
        if (i % 10000 == 0 || i == ITERATIONS - 1)
        {
            std::cout << "ops: " << i
                << " live: " << live.size() << " approx " << (liveSize / (1024.0 * 1024.0)) << "MB"
                << std::endl;
        }
    }

    // final cleanup + verification
    for (size_t i = 0; i < live.size(); i++)
    {
        auto a = live[i];
        verify_memory(a.ptr, a.size, a.pattern);
        rfree(a.ptr);

        // occasional progress
        if (i % 10000 == 0 || i == live.size() - 1)
        {
            std::cout << "freed " << i << std::endl;
        }
    }

    std::cout << "STRESS TEST PASSED\n";
}