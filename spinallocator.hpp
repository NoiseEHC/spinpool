//             Copyright Andrew Rafas 2013-2013.
// Distributed under the Boost Software License, Version 1.0.
//     (See accompanying file LICENSE_1_0.txt or copy at
//           http://www.boost.org/LICENSE_1_0.txt)

#include <array>
#include <cassert>
#include <atomic>
#include <mutex>
#include "spinlock.hpp"

using namespace std;

//typedef spinlock lock_type;
typedef mutex lock_type;

template<typename T, uint PageSize>
class spinallocator {
private:

    struct page {
        page *next; // nullptr terminated single list
        array<T,PageSize> items; // when in the full_list, always full, in empty_list always empty

        page() : next(nullptr) {
        }
    };

    class pagelist {
    private:
        page *head;
        //uint count;

//        void check_list_size() {
//            page *item = head;
//            uint cnt = 0;
//            while(item != nullptr) {
//                ++cnt;
//                item = item->next;
//            }
//            assert(cnt == count);
//        }

    public:
        pagelist() : head(nullptr) {
        }

        void insert(page *item) {
            //check_list_size();
            //++count;
            assert(item->next == nullptr);
            item->next = head;
            head = item;
            //check_list_size();
        }

        page *remove() {
            //check_list_size();
            //--count;
            page *result = head;
            if(result == nullptr) {
                assert(result != nullptr);
            }
            head = result->next;
            result->next = nullptr;
            //check_list_size();
            return result;
        }
    };

    static pagelist full_list;
    static pagelist empty_list;
    static lock_type static_lock;

    page *_low;
    page *_high;
    uint _full_count; // starts from 0 in _low, owerflows to _high (until reaches 2*PageSize)

public:
    static void init_empty_pages(uint page_count) {
        lock_guard<lock_type> lock(static_lock);
        for(uint i=0; i<page_count; ++i) {
            empty_list.insert(new page());
        }
    }

    static void init_full_page(const array<T,PageSize> &new_items) {
        lock_guard<lock_type> lock(static_lock);
        page *new_page = empty_list.remove();
        new_page->items = new_items;
        full_list.insert(new_page);
    }

    spinallocator() : _low(nullptr), _high(nullptr), _full_count(0) {
        lock_guard<lock_type> lock(static_lock);
        _low = full_list.remove();
        _high = empty_list.remove();
        _full_count = PageSize;
    }

    //TODO: in the desctructor we cannot free _low and _high back into free/empty_list as they can be partially filled...

    T alloc() {
        if(_full_count == 0) {
            lock_guard<lock_type> lock(static_lock);
            empty_list.insert(_low);
            _low = full_list.remove();
            _full_count = PageSize;
        }
        --_full_count;
        if(_full_count < PageSize) {
            return _low->items[_full_count];
        } else {
            return _high->items[_full_count-PageSize];
        }
    }

    void free(T value) {
        assert(_full_count <= 2*PageSize);
        if(_full_count >= 2*PageSize) {
            lock_guard<lock_type> lock(static_lock);
            full_list.insert(_high);
            _high = empty_list.remove();
            _full_count = PageSize;
        }
        if(_full_count < PageSize) {
            _low->items[_full_count] = value;
        } else {
            _high->items[_full_count-PageSize] = value;
        }
        ++_full_count;
    }
};

template<typename T, uint PageSize>
typename spinallocator<T, PageSize>::pagelist spinallocator<T, PageSize>::full_list;
template<typename T, uint PageSize>
typename spinallocator<T, PageSize>::pagelist spinallocator<T, PageSize>::empty_list;
template<typename T, uint PageSize>
lock_type spinallocator<T, PageSize>::static_lock;
