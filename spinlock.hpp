//             Copyright Andrew Rafas 2013-2013.
// Distributed under the Boost Software License, Version 1.0.
//     (See accompanying file LICENSE_1_0.txt or copy at
//           http://www.boost.org/LICENSE_1_0.txt)

#include <atomic>

#if defined(__GNUC__) || defined(__GNUG__)
#include <xmmintrin.h>
#endif

using namespace std;

class spinlock {
private:
    atomic_flag _taken;
    
public:
    spinlock() : _taken(ATOMIC_FLAG_INIT) {
    }
    
    void lock() {
		//ARGH: test_and_set returns the last value
        while(_taken.test_and_set(memory_order_acquire) == true)
            _mm_pause();
    }

    void unlock() {
        _taken.clear(memory_order_release);
    }
};

