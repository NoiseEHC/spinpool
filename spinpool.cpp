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
#include <chrono>

#if defined(__GNUC__) || defined(__GNUG__)
#include <xmmintrin.h>
#include <stdio.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
static __inline__ unsigned long long rdtsc(void)
{
    unsigned hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}
#endif

#include "spinallocator.hpp"

static_assert( sizeof(ulong) == 8, "64 bit needed" );
static_assert( sizeof(uint) == 4, "32 bit needed" );

using namespace std;
using namespace std::chrono;

unsigned int ReadThreadCount = 0;
unsigned int WriteThreadCount = 0;
unsigned int ReadWriteThreadCount = 0;
unsigned int ProcessingTime = 0;
ulong Total = 0;

const unsigned int RingBits = 20; // 1M
const ulong RingSize = ((ulong)1) << RingBits;
const ulong RingMask = RingSize-1;
const unsigned int CacheLineSizeBits = 3; // 2**3 * 8 byte = 64 byte
const ulong CacheLineSizeMask = ((((ulong)1) << CacheLineSizeBits)-1) << (RingBits-CacheLineSizeBits);
const uint PayloadBits = 10; // 1K
const uint PayloadShift = sizeof(ulong)*8 - PayloadBits;
const ulong PayloadMask = ~(((ulong)1 << PayloadShift)-1);

const uint PayloadCount = (uint)1 << PayloadBits;
const ulong ExitPayload = PayloadCount-1;
const uint AllocatorPageSize = 256;
const uint AllocatorFullPageCount = PayloadCount/AllocatorPageSize;
const uint AllocatorPageCount = AllocatorFullPageCount + 2*32; // assume max 32 threads
const uint AllocatorUsablePageCount = AllocatorFullPageCount - 2*32; // assume max 32 threads

atomic<bool> thread_start_sync(false);
atomic<ulong> total_writes;
atomic<ulong> total_reads;
atomic<ulong> running_writes;

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

	static ulong get_expected_full(ulong position) {
		return (position >> (RingBits-1)) | 1;
	}

public:
    static const ulong nothing_to_read = -1;
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
			ulong index = get_index(_position);
			//_m_prefetch(((char*)&_ring[index])+64);
			ulong expected_full = get_expected_full(_position);

			ulong value = _ring[index].load(memory_order_relaxed);
		try_again:
		    ulong counter_value = value & ~PayloadMask;
			assert(counter_value >= expected_full-2);

			if(counter_value < expected_full) {
				++retry1;
				return nothing_to_read;
			}

			if(counter_value == expected_full) {
			    //TOOD: it is a little bit slower than using intrinsics (on x64 gcc at least)
			    if(_ring[index].compare_exchange_weak(value, counter_value+1, memory_order_acquire, memory_order_relaxed)) {
					++_position;
					return value >> PayloadShift;
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
					value = _ring[index].load(memory_order_relaxed);
        		    counter_value = value & ~PayloadMask;
					if(counter_value < expected_full+2)
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

	static ulong get_expected_empty(ulong position) {
		return (position >> (RingBits-1)) & ~(ulong)1;
	}
	
	spinallocator<ulong,AllocatorPageSize> _allocator;

public:
	ulong retry1;
	char _padding2[64];

	Writer(atomic<ulong> *ring) {
		_position = 2*RingSize;
		_ring = ring;
		retry1 = 0;
	}

	void write(ulong payload) {
		ulong index = get_index(_position);
		//_m_prefetch(((char*)&_ring[index])+64);
		ulong expected_empty = get_expected_empty(_position);
		
		while(_ring[index].load(memory_order_relaxed) != expected_empty) {
		    ++retry1;
		    _mm_pause();
		}
		ulong value = payload << PayloadShift | (expected_empty+1);
		_ring[index].store(value, memory_order_release);

		++_position;
	}
};

class MultiReader {
public:
    static const ulong nothing_to_read = Reader::nothing_to_read;
    
    struct ReaderWithCount {
        Reader reader;
        ulong count;
        
        ReaderWithCount(Reader reader, ulong count) : reader(reader), count(count) {}
    };
    
    vector<ReaderWithCount> readers;
    
    MultiReader(uint preferred_writer, uint writer_count) {
        //TODO: first loop through our cpu socket then the other sockets
        for(uint i=0; i<writer_count; ++i)
            readers.push_back(ReaderWithCount(Reader(AllRings[(i+preferred_writer) % writer_count]),0));
    }

	ulong read() {
        for(auto& r : readers) {
            ulong value = r.reader.read();
            if(value != nothing_to_read) {
                ++r.count;
                return value;
            }
        }
        return nothing_to_read;
	}

	ulong blocking_read() {
	    while(true) {
	        ulong value = read();
            if(value != nothing_to_read)
                return value;
            _mm_pause();
	    }
	}
};

void read_thread(int index) {
    try_set_affinity(index+WriteThreadCount);
    MultiReader mr(index, WriteThreadCount);
	wait_for_thread_start();
	while(true) {
        ulong value = mr.read();
        if(value != MultiReader::nothing_to_read) {
            if(value == ExitPayload)
                break;
            if(value != 1) {
                printf("read value: %u\n", (uint)value);
            }
	        wait_pause(ProcessingTime);
        } else {
            _mm_pause();
        }
	}
	uint i=0;
    for(auto &r : mr.readers) {
    	printf("Read %d/%d: %.3f (%" PRIu64 "), Retry: %.3f %.3f, Multiskip: %" PRIu64 "\n", index, i, r.count/1000000.0, r.count, r.reader.retry1/1000000.0, r.reader.retry2/1000000.0, r.reader.multiskip);
        total_reads += r.count;
    	++i;
    }
}

void write_thread(int index) {
    try_set_affinity(index);
	Writer w(AllRings[index]);
	wait_for_thread_start();
	ulong success = 0;
	ulong total = Total/WriteThreadCount;
	for(ulong count = 0; count < total; ++count) {
		w.write(1);
		++success;
	}
    if(--running_writes == 0) {
	    for(uint i=0; i<ReadThreadCount; ++i)
        	w.write(ExitPayload);
    }
	printf("Written: %.3f (%" PRIu64 ") Retry: %.3f\n", success/1000000.0, success, w.retry1/1000000.0);
	total_writes += success;
}

void read_write_thread(int index) {
    // there is better to be no readers and writers!!!
    try_set_affinity(index);
    MultiReader mr(index, ReadWriteThreadCount);
	Writer w(AllRings[index]);
	wait_for_thread_start();
	uint multiplier = 2;
	if(index == 0) {
    	/*ulong total = Total;
	    for(ulong i=0; i<total; ++i) {
       		wait_pause(ProcessingTime); // 20 means 5 million/sec on my laptop
	    }
        total_writes += total;
	    return;*/
    	ulong total = Total/multiplier;
    	ulong write_success=0;
        while(write_success < total) {
            w.write(1); ++write_success;
       		wait_pause(ProcessingTime);
	    }
       	total_writes += write_success;
	    for(uint i=0; i<ReadWriteThreadCount; ++i)
        	w.write(1000000);
    	printf("Written %d: %.3f (%" PRIu64 ") Retry: %.3f\n", index, write_success/1000000.0, write_success, w.retry1/1000000.0);
	} else {
	    ulong write_success = 0;
	    while(true) {
       		ulong value = mr.blocking_read();
       		if(value == 1000000)
       		    break;
       		if(value == 1) {
	            for(uint i=0; i<multiplier; ++i) {
		            w.write(2); ++write_success;
		        }
		    } // else swallow the value 2*/
       		//wait_pause(ProcessingTime);
	    }
	    uint i=0;
        for(auto &r : mr.readers) {
        	printf("Read %d/%d: %.3f (%" PRIu64 "), Retry: %.3f %.3f, Multiskip: %" PRIu64 "\n", index, i, r.count/1000000.0, r.count, r.reader.retry1/1000000.0, r.reader.retry2/1000000.0, r.reader.multiskip);
            total_reads += r.count;
        	++i;
        }
	    printf("Written %d: %.3f (%" PRIu64 ") Retry: %.3f\n", index, write_success/1000000.0, write_success, w.retry1/1000000.0);
	    total_writes += write_success;
	}
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
	spinallocator<ulong,AllocatorPageSize>::init_empty_pages(AllocatorPageCount);
	for(ulong i=0; i<AllocatorFullPageCount; ++i) {
	    ulong start = i*AllocatorPageSize;
	    array<ulong,AllocatorPageSize> items;
	    for(uint j=0; j<AllocatorPageSize; ++j)
	        items[j] = start+j;
        spinallocator<ulong,AllocatorPageSize>::init_full_page(items);
	}
	spinallocator<ulong,AllocatorPageSize> alloc;
	/*for(ulong i=0; i<AllocatorUsablePageCount*AllocatorPageSize; ++i) {
	    ulong item = alloc.alloc();
	    printf("%d ", (uint)item);
	    assert(item == AllocatorFullPageCount*AllocatorPageSize-1-i);
    }*/	    

	printf("starting with %" PRIu64 " ops, read: %d, write: %d, read/write: %d, pause loops: %d\n", Total, ReadThreadCount, WriteThreadCount, ReadWriteThreadCount, ProcessingTime);
	list<thread> threads;
	running_writes = WriteThreadCount;
	for(unsigned int i=0; i<ReadThreadCount; ++i) {
		threads.push_back(thread([=]{ read_thread(i); }));
	}
	for(unsigned int i=0; i<WriteThreadCount; ++i) {
		threads.push_back(thread([=]{ write_thread(i); }));
	}
	for(unsigned int i=0; i<ReadWriteThreadCount; ++i) {
		threads.push_back(thread([=]{ read_write_thread(i); }));
	}
	auto start = steady_clock::now();
	thread_start_sync = true;
	for(thread &t : threads)
		t.join();
	auto end = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start);
	printf("%" PRIu64 " write ops, %" PRIu64 " read ops, %.3f million queue ops/sec\n", total_writes.load(), total_reads.load(), (double)total_writes.load() / (double)elapsed.count() * 1000.0 / 1000000.0);
	return 0;
}
