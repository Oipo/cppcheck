/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2026 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "blockpool.h"
#include "config.h"
#include "fixture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#ifdef HAS_THREADING_MODEL_THREAD
#include <thread>
#endif

class TestBlockPool : public TestFixture {
public:
    TestBlockPool() : TestFixture("TestBlockPool") {}

private:
    // every test has its own types, so it starts with an empty pool
    template<int N, std::size_t Size = 40>
    struct Object {
        alignas(std::max_align_t) unsigned char data[Size];
    };

    void run() override {
        TEST_CASE(sequential);
        TEST_CASE(reuse);
        TEST_CASE(manyChunks);
        TEST_CASE(tinyObject);
        TEST_CASE(threadEnd);
        TEST_CASE(freedByOtherThread);
    }

    template<class T>
    static std::vector<void*> allocate(std::size_t count) {
        std::vector<void*> blocks;
        for (std::size_t i = 0; i < count; ++i) {
            blocks.push_back(BlockPool<T>::allocate());
            std::memset(blocks.back(), static_cast<int>(i), sizeof(T));
        }
        return blocks;
    }

    template<class T>
    static void deallocate(const std::vector<void*>& blocks) {
        for (void* p : blocks)
            BlockPool<T>::deallocate(p);
    }

    static std::uintptr_t address(const void* p) {
        return reinterpret_cast<std::uintptr_t>(p);
    }

    void sequential() const {
        using T = Object<0>;
        const std::vector<void*> blocks = allocate<T>(100);
#if !ASAN
        // next to each other, in the order of the allocation
        for (std::size_t i = 1; i < blocks.size(); ++i)
            ASSERT_EQUALS(address(blocks[i - 1]) + sizeof(T), address(blocks[i]));
#endif
        for (const void* p : blocks)
            ASSERT_EQUALS(0, address(p) % alignof(T));
        deallocate<T>(blocks);
    }

    void reuse() const {
        using T = Object<1>;
        void* a = BlockPool<T>::allocate();
        void* b = BlockPool<T>::allocate();
        ASSERT(a != b);
        BlockPool<T>::deallocate(a);
#if !ASAN
        // the last freed block comes first
        ASSERT_EQUALS(a, BlockPool<T>::allocate());
#else
        a = BlockPool<T>::allocate();
#endif
        BlockPool<T>::deallocate(a);
        BlockPool<T>::deallocate(b);
    }

    void manyChunks() const {
        using T = Object<2, 1000>;
        // more than 3 chunks of 64 KiB
        const std::vector<void*> blocks = allocate<T>(200);
        const std::set<void*> distinct(blocks.cbegin(), blocks.cend());
        ASSERT_EQUALS(blocks.size(), distinct.size());
        // the blocks do not overlap
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const auto* data = static_cast<const unsigned char*>(blocks[i]);
            ASSERT_EQUALS(static_cast<unsigned char>(i), data[0]);
            ASSERT_EQUALS(static_cast<unsigned char>(i), data[sizeof(T) - 1]);
        }
        deallocate<T>(blocks);
#if !ASAN
        // all the blocks come back, without new chunks
        const std::vector<void*> again = allocate<T>(200);
        ASSERT(distinct == std::set<void*>(again.cbegin(), again.cend()));
        deallocate<T>(again);
#endif
    }

    void tinyObject() const {
        // a block also holds the links of the free lists
        using T = char;
        const std::vector<void*> blocks = allocate<T>(10);
        const std::set<void*> distinct(blocks.cbegin(), blocks.cend());
        ASSERT_EQUALS(blocks.size(), distinct.size());
        deallocate<T>(blocks);
    }

    void threadEnd() const {
#ifdef HAS_THREADING_MODEL_THREAD
        using T = Object<3>;
        std::vector<void*> blocks;
        std::thread([&blocks]() {
            blocks = allocate<T>(10);
            deallocate<T>(blocks);
        }).join();
#if !ASAN
        // the free list of the ended thread is in the reserve
        const std::set<void*> freed(blocks.cbegin(), blocks.cend());
        const std::vector<void*> again = allocate<T>(10);
        ASSERT(freed == std::set<void*>(again.cbegin(), again.cend()));
#else
        const std::vector<void*> again = allocate<T>(10);
#endif
        deallocate<T>(again);
#endif
    }

    void freedByOtherThread() const {
#ifdef HAS_THREADING_MODEL_THREAD
        using T = Object<4>;
        std::vector<void*> blocks;
        std::thread([&blocks]() {
            blocks = allocate<T>(10);
        }).join();
        std::thread([&blocks]() {
            deallocate<T>(blocks);
        }).join();
        // the blocks went to the free list of the second thread, and then to the reserve
        std::vector<void*> again;
        std::thread([&again]() {
            again = allocate<T>(10);
        }).join();
#if !ASAN
        const std::set<void*> freed(blocks.cbegin(), blocks.cend());
        ASSERT(freed == std::set<void*>(again.cbegin(), again.cend()));
#endif
        deallocate<T>(again);
#endif
    }
};

REGISTER_TEST(TestBlockPool)
