/* -*- C++ -*-
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

#ifndef blockpoolH
#define blockpoolH

#include "config.h"

#include <cstddef>
#include <mutex>
#include <new>

/**
 * Memory for objects of type T, taken from chunks of many blocks of sizeof(T) bytes.
 *
 * Objects that are allocated one after the other lie next to each other, apart from the objects of
 * other types. This keeps the walks over linked structures, such as the token list, in the caches
 * of the processor. With malloc the tokens of a large file are spread over the heap.
 *
 * Every thread has its own list of free blocks, so allocating and freeing need no lock. A block that
 * one thread frees joins the free list of that thread, whichever thread allocated it. When a thread
 * ends, its free list goes to a shared reserve, from which the threads take blocks before a new chunk
 * is allocated. The chunks are never returned to the system.
 *
 * With the address sanitizer the blocks come from operator new, so that it still finds the uses
 * after free.
 */
template<class T>
class BlockPool {
public:
    /** Memory for one T */
    static void* allocate() {
#if ASAN
        return ::operator new(sizeof(T));
#else
        Local& local = localStorage();
        if (!local.free) {
            Block* list = reserve().take();
            if (endedThread()) {
                // the free list of this thread is not returned again
                reserve().give(list->links.next);
                return list;
            }
            local.free = list;
        }
        Block* block = local.free;
        local.free = block->links.next;
        return block;
#endif
    }

    /** Free the memory of allocate() */
    static void deallocate(void* p) noexcept {
#if ASAN
        ::operator delete(p);
#else
        Block* block = static_cast<Block*>(p);
        if (endedThread()) {
            reserve().giveOne(block);
            return;
        }
        Local& local = localStorage();
        block->links.next = local.free;
        local.free = block;
#endif
    }

private:
    union Block {
        struct Links {
            Block* next;
            /** In the reserve: the next free list */
            Block* nextList;
        } links;
        alignas(T) unsigned char storage[sizeof(T)];
    };
    static_assert(alignof(Block) <= alignof(std::max_align_t), "the chunks come from operator new");

    class Reserve {
    public:
        /** A free list, never empty */
        Block* take() {
            std::lock_guard<std::mutex> lock(mMutex);
            Block* list = mLists;
            if (list) {
                mLists = list->links.nextList;
                return list;
            }
            return newChunk();
        }

        void give(Block* list) noexcept {
            if (!list)
                return;
            std::lock_guard<std::mutex> lock(mMutex);
            list->links.nextList = mLists;
            mLists = list;
        }

        void giveOne(Block* block) noexcept {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mLists) {
                block->links.next = mLists->links.next;
                mLists->links.next = block;
            } else {
                block->links.next = nullptr;
                block->links.nextList = nullptr;
                mLists = block;
            }
        }

    private:
        Block* newChunk() {
            constexpr std::size_t blocksPerChunk = (65536 / sizeof(Block)) > 1 ? (65536 / sizeof(Block)) : 1;
            // the first block links the chunks, so they stay reachable for the leak checkers
            Block* chunk = static_cast<Block*>(::operator new((blocksPerChunk + 1) * sizeof(Block)));
            chunk->links.next = mChunks;
            mChunks = chunk;
            // in the order of the addresses
            for (std::size_t i = 1; i < blocksPerChunk; ++i)
                chunk[i].links.next = &chunk[i + 1];
            chunk[blocksPerChunk].links.next = nullptr;
            return &chunk[1];
        }

        std::mutex mMutex;
        Block* mLists{};
        Block* mChunks{};
    };

    struct Local {
        Block* free;
        bool ended;
    };

    /** Gives the free list of the thread to the reserve when the thread ends */
    struct Ender {
        Ender() = default;
        Ender(const Ender&) = delete;
        Ender& operator=(const Ender&) = delete;
        ~Ender() {
            Local& local = localStorage();
            reserve().give(local.free);
            local.free = nullptr;
            local.ended = true;
        }
    };

    static Local& localStorage() noexcept {
        // trivially destructible, so it can be used while the thread ends
        static thread_local Local local{};
        return local;
    }

    /** Whether the Ender of this thread has run, which registers it on the first call */
    static bool endedThread() noexcept {
        static thread_local Ender ender;
        (void)ender;
        return localStorage().ended;
    }

    static Reserve& reserve() {
        // never destroyed, so that blocks can be freed in the destructors of static objects
        static Reserve* const r = new Reserve;
        return *r;
    }
};

#endif // blockpoolH
