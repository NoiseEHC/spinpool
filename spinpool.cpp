//             Copyright Andrew Rafas 2013-2013.
// Distributed under the Boost Software License, Version 1.0.
//     (See accompanying file LICENSE_1_0.txt or copy at
//           http://www.boost.org/LICENSE_1_0.txt)

#if defined(_MSC_VER)
#error "Both VS 2012/2013 compile lock cmpxchg8 for atomic<T>.load(), do not use them!!!"
#endif

#include <list>
#include <vector>
#include <atomic>
#include <thread>
#include <cassert>

#if defined(__GNUC__) || defined(__GNUG__)
#include <xmmintrin.h>
#include <stdio.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <sys/time.h>
int GetTickCount()
{
	struct timeval  tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec) * 1000 + (tv.tv_usec / 1000) ;
}
#endif

static_assert( sizeof(ulong) == 8, "64 bit needed" );
static_assert( sizeof(uint) == 4, "32 bit needed" );

using namespace std;

unsigned int ReadThreadCount = 0;
unsigned int WriteThreadCount = 0;
unsigned int ReadWriteThreadCount = 0;
unsigned int ProcessingTime = 0;
const unsigned int RingBits = 10; // 1M
const ulong RingSize = ((ulong)1) << RingBits;
const ulong RingMask = RingSize-1;
ulong Total = 0;
const unsigned int CacheLineSizeBits = 3; // 2**3 * 8 byte = 64 byte
const ulong CacheLineSizeMask = ((((ulong)1) << CacheLineSizeBits)-1) << (RingBits-CacheLineSizeBits);

atomic<bool> thread_start_sync(false);
atomic<ulong> total_writes;

const unsigned int MaxRingCount = 32;
//vector< vector< atomic<ulong> > > AllRings;
atomic<ulong> AllRings[MaxRingCount][RingSize];

void wait_for_thread_start() {
	while(!thread_start_sync)
		_mm_pause();
}

void wait_pause(int count) {
	for(int i=0; i<count; ++i)
		_mm_pause();
}

ulong get_index(ulong position) {
	//return position & RingMask;
	return ((position & (RingMask & ~CacheLineSizeMask)) << CacheLineSizeBits) |
		((position & CacheLineSizeMask) >> (RingBits-CacheLineSizeBits));
}

void try_set_affinity(int core_index) {
    return;
#if defined(_MSC_VER)
	//printf("Old affinity for %d: %d\n", index, SetThreadAffinityMask(GetCurrentThread(), 1<<index));
#endif
    unsigned long mask = 1 << core_index;
    if(pthread_setaffinity_np(pthread_self(), sizeof(mask), (cpu_set_t*)&mask) < 0)
        printf("setting affinity to %d failed\n", core_index);
}

class Reader {
private:
	char _padding1[64];
	ulong _position;
	atomic<ulong> *_ring;

	ulong get_index() {
		return ::get_index(_position);
	}

	static ulong get_expected_full(ulong position) {
		return (position >> (RingBits-1)) | 1;
	}

	ulong get_expected_full() {
		return get_expected_full(_position);
	}

public:
	ulong retry1;
	ulong retry2;
	ulong multiskip;
	char _padding2[64];

	Reader(atomic<ulong> *ring) {
		_position = 2*RingSize;
		_ring = ring;
		retry1 = 0;
		retry2 = 0;
		multiskip = 0;
	}

	ulong read() {
		while(true) {
			ulong index = get_index();
			//_m_prefetch(((char*)&_ring[index])+64);
			ulong expected_full = get_expected_full();

			ulong value = _ring[index].load(memory_order_relaxed);
		try_again:
			assert(value >= expected_full-2);

			if(value < expected_full) {
				++retry1;
				return -1;
			}

			if(value == expected_full) {
			    //TOOD: it is a little bit slower than using intrinsics (on x64 gcc at least)
			    if(_ring[index].compare_exchange_weak(value, value+1, memory_order_acquire, memory_order_relaxed)) {
					++_position;
					return value;
				}
				++retry2;
				goto try_again;
			} else { // skip or we are behind
				ulong skip_size = 1;
				//TODO: optimize case when we are behing with at least a round (different than skipping 1-32 items...)
				while(true) {
					ulong test_position = _position+skip_size*2;
					index = ::get_index(test_position);
					expected_full = get_expected_full(test_position);
					if(_ring[index].load(memory_order_relaxed) < expected_full+2)
						break;
					skip_size *= 2;
					++multiskip;
				}
				_position += skip_size;
			}
		}
	}
};

class Writer {
private:
	char _padding1[64];
	ulong _position;
	atomic<ulong> *_ring;

	ulong get_index() {
		return ::get_index(_position);
	}

	static ulong get_expected_empty(ulong position) {
		return (position >> (RingBits-1)) & ~(ulong)1;
	}

	ulong get_expected_empty() {
		return get_expected_empty(_position);
	}

public:
	ulong retry1;
	ulong retry2;
	ulong multiskip;
	char _padding2[64];

	Writer(atomic<ulong> *ring) {
		_position = 2*RingSize;
		_ring = ring;
		retry1 = 0;
		retry2 = 0;
		multiskip = 0;
	}

	void write() {
		ulong index = get_index();
		//_m_prefetch(((char*)&_ring[index])+64);
		ulong expected_empty = get_expected_empty();
		
		while(_ring[index].load(memory_order_relaxed) != expected_empty)
		    _mm_pause();
		_ring[index].store(expected_empty+1, memory_order_release);

		++_position;
	}
};

class MultiReader {
public:
    vector<Reader> readers;
    
    MultiReader(uint preferred_writer, uint writer_count) {
        //TODO: first loop through our cpu socket then the other sockets
        for(uint i=0; i<writer_count; ++i)
            readers.push_back(Reader(AllRings[(i+preferred_writer) % writer_count]));
    }

	ulong read() {
        for(auto& r : readers) {
            ulong value = r.read();
            if(value != (ulong)-1)
                return value;
        }
        return -1;
	}
};

atomic<unsigned int> running_writers;

void read_thread(int index) {
    try_set_affinity(index+WriteThreadCount);
    MultiReader mr(index, WriteThreadCount);
	ulong success = 0;
	wait_for_thread_start();
	while(running_writers.load(memory_order_relaxed) != 0) {
        ulong value = mr.read();
        if(value != (ulong)-1) {
	        ++success;
	        wait_pause(ProcessingTime);
        } else {
            _mm_pause();
        }
	}
    for(unsigned int i=0; i<WriteThreadCount; ++i) {
        auto &r = mr.readers[i];
    	printf("Read %d/%d: %.3f (%" PRIu64 "), Retry: %.3f %.3f, Multiskip: %" PRIu64 "\n", index, i, success/1000000.0, success, r.retry1/1000000.0, r.retry2/1000000.0, r.multiskip);
    }
}

void write_thread(int index) {
    try_set_affinity(index);
	Writer w(AllRings[index]);
	wait_for_thread_start();
	ulong success = 0;
	ulong total = Total/WriteThreadCount+1;
	for(ulong count = 0; count < total; ++count) {
		w.write();
		++success;
	}
	--running_writers;
	printf("Written: %.3f (%" PRIu64 ") Retry: %.3f %.3f, Multiskip: %" PRIu64 "\n", success/1000000.0, success, w.retry1/1000000.0, w.retry2/1000000.0, w.multiskip);
	total_writes += success;
}

void read_write_thread(int index) {
    try_set_affinity(index+WriteThreadCount+ReadThreadCount);
	Reader r(AllRings[index]);
	Writer w(AllRings[index]);
	wait_for_thread_start();
	ulong read_success = 0;
	ulong write_success = 0;
	for(int i=0; i<1; ++i) // readers and writers are separated by as many items (must be at least 1 !!!)
		w.write();
	ulong total = Total*2/3/ReadWriteThreadCount;
	for(ulong count = 0; count < total; ++count) {
		//printf("%d %d\n", index, count);
		if(count & 1) {
			r.read(); ++read_success;
			r.read(); ++read_success;
			w.write(); ++write_success;
		} else {
			r.read(); ++read_success;
			w.write(); ++write_success;
			w.write(); ++write_success;
		}
		wait_pause(ProcessingTime);
	}
	printf("%d Read   : %.3f (%" PRIu64 ") Retry: %.3f %.3f, Multiskip: %" PRIu64 "\n", index, read_success/1000000.0, read_success, r.retry1/1000000.0, r.retry2/1000000.0, r.multiskip);
	printf("%d Written: %.3f (%" PRIu64 ") Retry: %.3f %.3f, Multiskip: %" PRIu64 "\n", index, write_success/1000000.0, write_success, w.retry1/1000000.0, w.retry2/1000000.0, w.multiskip);
	total_writes += write_success;
}

int main(int argc, char* argv[])
{
	if(argc != 6) {
		printf("Usage: disruptor_cpp <iterations in millions> <read thread count> <write thread count> <read/write thread count> <processing time>\n");
		return 1;
	}
	Total = 1000000LU * atoi(argv[1]);
	ReadThreadCount = atoi(argv[2]);
	WriteThreadCount = atoi(argv[3]);
	ReadWriteThreadCount = atoi(argv[4]);
	ProcessingTime = atoi(argv[5]);

	for(unsigned int j=0; j<MaxRingCount; ++j) {
	    for(unsigned int i=0; i<RingSize; ++i) {
		    AllRings[j][i] = 4; // starting with 0 causes the asserts to fail because of the underflow of unsigned values
	    }
	}
	printf("starting with %" PRIu64 " ops, read: %d, write: %d, read/write: %d, pause loops: %d\n", Total, ReadThreadCount, WriteThreadCount, ReadWriteThreadCount, ProcessingTime);
	list<thread> threads;
	running_writers = WriteThreadCount;
	for(unsigned int i=0; i<ReadThreadCount; ++i) {
		threads.push_back(thread([=]{ read_thread(i); }));
	}
	for(unsigned int i=0; i<WriteThreadCount; ++i) {
		threads.push_back(thread([=]{ write_thread(i); }));
	}
	for(unsigned int i=0; i<ReadWriteThreadCount; ++i) {
		threads.push_back(thread([=]{ read_write_thread(i); }));
	}
	auto start = GetTickCount();
	thread_start_sync = true;
	for(thread &t : threads)
		t.join();
	auto elapsed = GetTickCount() - start;
	printf("%" PRIu64 " write ops, %.3f million ops/sec\n", total_writes.load(), (double)total_writes.load() / (double)elapsed * 1000.0 / 1000000.0);
	return 0;
}
