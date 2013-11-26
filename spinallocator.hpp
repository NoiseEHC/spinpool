//             Copyright Andrew Rafas 2013-2013.
// Distributed under the Boost Software License, Version 1.0.
//     (See accompanying file LICENSE_1_0.txt or copy at
//           http://www.boost.org/LICENSE_1_0.txt)

#include <array>
#include <cassert>
#include <mutex>
#include "spinlock.hpp"

using namespace std;

template<typename T, uint PageSize> 
class spinallocator {
private:
    
    
    struct page {
        page *next; // nullptr terminated single list
        array<T,PageSize> items; // when in the full_list, always full, in empty_list always empty
        
        page() : next(nullptr) {
        }
    };
    
    static page *full_list;
    static page *empty_list;
    static spinlock static_lock;
    
    page *_low;
    page *_high;
    uint _full_count; // starts from 0 in _low, owerflows to _high (until reaches 2*PageSize)
    
    static void insert_into_list(page* &list, page *item) {
        assert(item->next == nullptr);
        item->next = list;
        list = item;
    }
    
    static page *remove_from_list(page* &list) {
        page *result = list;
        assert(result != nullptr);
        list = result->next;
        result->next = nullptr;
        return result;
    }

public:
    static void init_empty_pages(uint page_count) {
        lock_guard<spinlock> lock(static_lock);
        for(uint i=0; i<page_count; ++i)
            insert_into_list(empty_list, new page());
    }
    
    static void init_full_page(const array<T,PageSize> &new_items) {
        lock_guard<spinlock> lock(static_lock);
        page *new_page = remove_from_list(empty_list);
        new_page->items = new_items;
        insert_into_list(full_list, new_page);
    }
    
    spinallocator() : _low(nullptr), _high(nullptr), _full_count(0) {
        lock_guard<spinlock> lock(static_lock);
        _low = remove_from_list(full_list);
        _high = remove_from_list(empty_list);
        _full_count = PageSize;
    }
    
    //TODO: in the desctructor we cannot free _low and _high back into free/empty_list as they can be partially filled...
    
    T alloc() {
        if(_full_count == 0) {
            lock_guard<spinlock> lock(static_lock);
            insert_into_list(empty_list, _low);
            _low = remove_from_list(full_list);
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
            lock_guard<spinlock> lock(static_lock);
            insert_into_list(full_list, _high);
            _high = remove_from_list(empty_list);
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
typename spinallocator<T, PageSize>::page *spinallocator<T, PageSize>::full_list = nullptr;
template<typename T, uint PageSize> 
typename spinallocator<T, PageSize>::page *spinallocator<T, PageSize>::empty_list = nullptr;
template<typename T, uint PageSize> 
spinlock spinallocator<T, PageSize>::static_lock;

