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

uint ReadThreadCount = 0;
uint WriteThreadCount = 0;
uint ReadProcessingTime = 0;
uint WriteProcessingTime = 0;
ulong Total = 0;

const uint RingBits = 10; // 1K
const ulong RingSize = ((ulong)1) << RingBits;
const ulong RingMask = RingSize-1;
const uint CacheLineSizeBits = 3; // 2**3 * 8 byte = 64 byte
const ulong CacheLineSizeMask = ((((ulong)1) << CacheLineSizeBits)-1) << (RingBits-CacheLineSizeBits);
const uint PayloadBits = 24; // 2K
const uint PayloadShift = sizeof(ulong)*8 - PayloadBits;
const ulong PayloadMask = ~(((ulong)1 << PayloadShift)-1);

const uint PayloadCount = (uint)1 << (PayloadBits-1);
const ulong ExitPayload = ((uint)1 << PayloadBits)-1;
const uint AllocatorPageSize = 256;
const uint AllocatorFullPageCount = PayloadCount/AllocatorPageSize;
const uint AllocatorPageCount = AllocatorFullPageCount + 2*32; // assume max 32 threads
const uint AllocatorUsablePageCount = AllocatorFullPageCount - 2*32; // assume max 32 threads

atomic<ulong> thread_start_sync;
atomic<ulong> total_writes;
atomic<ulong> total_reads;
atomic<ulong> running_writes;

void wait_for_thread_start() {
	ulong wait_count = ReadThreadCount+WriteThreadCount;
	++thread_start_sync;
	while(thread_start_sync != wait_count)
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

const uint CacheLineSize = 64;
const uint PaddingSize = CacheLineSize-sizeof(void*);

class RingBuffer {
private:
	char _padding1[PaddingSize];
	atomic<ulong> *_ring;
	ulong _mask;

	char _padding2[PaddingSize];
	ulong _read_position;

	char _padding3[PaddingSize];
	ulong _write_position;

	char _padding4[PaddingSize];

public:
	RingBuffer(uint bit_count) {
		uint size = 1<<bit_count;
		_ring = new atomic<ulong>[size];
		_mask = size-1;
		_read_position = 0;
		_write_position = 0;
	}
	
	void write(ulong value) {
		ulong index = _write_position & _mask;
		//ulong index = ::get_index(_write_position);
		auto p = _ring+index;
		while(p->load(memory_order_relaxed) != 0)
			_mm_pause();
		p->store(value, memory_order_release);
		++_write_position;
	}

	ulong try_read() {
		ulong index = _read_position & _mask;
		//ulong index = ::get_index(_read_position);
		auto p = _ring+index;
		ulong value = p->load(memory_order_acquire);
		if(value == 0)
			return 0;
		p->store(0, memory_order_relaxed);
		++_read_position;
		return value;
	}
	
	ulong read() {
		while(true) {
			ulong value = try_read();
			if(value != 0) {
				return value;
			} else {
				_mm_pause();
			}
		}
	}
};

class MultiReader {
public:
    struct ReaderWithCount {
        RingBuffer *reader;
        ulong count;
        
        ReaderWithCount(RingBuffer *reader, ulong count) : reader(reader), count(count) {}
    };
    
    vector<ReaderWithCount> readers;

    MultiReader(vector<RingBuffer*> &buffers, uint preferred_writer) {
        //TODO: first loop through our cpu socket then the other sockets
        for(uint i=0; i<buffers.size(); ++i)
            readers.push_back(ReaderWithCount(buffers[(i+preferred_writer) % buffers.size()], 0));
    }

	ulong try_read() {
        for(auto& r : readers) {
            ulong value = r.reader->try_read();
            if(value != 0) {
                ++r.count;
                return value;
            }
        }
        return 0;
	}

	ulong read() {
	    while(true) {
	        ulong value = try_read();
            if(value != 0)
                return value;
            _mm_pause();
	    }
	}
};

class MultiWriter {
public:
    struct WriterWithCount {
        RingBuffer *writer;
        ulong count;
        
        WriterWithCount(RingBuffer *writer, ulong count) : writer(writer), count(count) {}
    };
    
    vector<WriterWithCount> writers;
	uint write_index;

    MultiWriter(vector<RingBuffer*> &buffers) {
        //TODO: first loop through our cpu socket then the other sockets
        for(uint i=0; i<buffers.size(); ++i)
            writers.push_back(WriterWithCount(buffers[i], 0));
		write_index = 0;
    }

	void write(ulong value) {
		writers[write_index].writer->write(value);
		++writers[write_index].count;
		++write_index;
		if(write_index >= writers.size())
			write_index = 0;
	}

	void write_to_all(ulong value) {
		for(auto &w : writers) {
			w.writer->write(value);
			++w.count;
		}
	}
};

vector<RingBuffer> rb_array;

static RingBuffer *get_for_writer_reader(uint writer_index, uint reader_index) {
	return &rb_array[writer_index * ReadThreadCount + reader_index];
}

void read_thread(int index) {
    try_set_affinity(index+WriteThreadCount);
	spinallocator<ulong,AllocatorPageSize> alloc;
	wait_for_thread_start();
	vector<RingBuffer*> writers;
	for(uint i=0; i<WriteThreadCount; ++i)
		writers.push_back(get_for_writer_reader(i, index));
	MultiReader mr(writers, index);
	while(true) {
        ulong value = mr.read();
		if(value == ExitPayload)
			break;
//      alloc.free(value);
        wait_pause(ReadProcessingTime);
	}
	uint i=0;
    for(auto &r : mr.readers) {
    	printf("Read %d/%d: %.3f (%" PRIu64 ")\n", index, i, r.count/1000000.0, r.count);
        total_reads += r.count;
    	++i;
    }
}

void write_thread(int index) {
    try_set_affinity(index);
	spinallocator<ulong,AllocatorPageSize> alloc;
	wait_for_thread_start();
	vector<RingBuffer*> readers;
	for(uint i=0; i<ReadThreadCount; ++i)
		readers.push_back(get_for_writer_reader(index, i));
	MultiWriter mw(readers);
	ulong success = 0;
	ulong total = Total/WriteThreadCount;
	for(ulong count = 0; count < total; ++count) {
//	    ulong data = alloc.alloc();
//		w.write(data);
		++success;
		mw.write(1);
        wait_pause(WriteProcessingTime);
	}
    if(--running_writes == 0) {
		mw.write_to_all(ExitPayload);
    }
	printf("Written: %.3f (%" PRIu64 ")\n", success/1000000.0, success);
	total_writes += success;
}

int main(int argc, char* argv[])
{
	if(argc != 6) {
		printf("Usage: disruptor_cpp <iterations in millions> <read thread count> <write thread count> <read processing time> <write processing time>\n");
		return 1;
	}
	Total = 1000000LU * atoi(argv[1]);
	ReadThreadCount = atoi(argv[2]);
	WriteThreadCount = atoi(argv[3]);
	ReadProcessingTime = atoi(argv[4]);
	WriteProcessingTime = atoi(argv[5]);

	spinallocator<ulong,AllocatorPageSize>::init_empty_pages(AllocatorPageCount);
	for(ulong i=0; i<AllocatorFullPageCount; ++i) {
	    ulong start = i*AllocatorPageSize;
	    array<ulong,AllocatorPageSize> items;
	    for(uint j=0; j<AllocatorPageSize; ++j)
	        items[j] = start+j;
        spinallocator<ulong,AllocatorPageSize>::init_full_page(items);
	}
	/*spinallocator<ulong,AllocatorPageSize> alloc;
	for(ulong i=0; i<AllocatorUsablePageCount*AllocatorPageSize; ++i) {
	    ulong item = alloc.alloc();
	    printf("%d ", (uint)item);
	    assert(item == AllocatorFullPageCount*AllocatorPageSize-1-i);
    }*/	  
    printf("%u %u %u\n", AllocatorFullPageCount, AllocatorPageCount, AllocatorUsablePageCount);

	for(uint i=0; i<ReadThreadCount*WriteThreadCount; ++i)
		rb_array.push_back(RingBuffer(RingBits));
		
	printf("starting with %" PRIu64 " ops, read: %d, write: %d, read time: %d, write time: %d\n", Total, ReadThreadCount, WriteThreadCount, ReadProcessingTime, WriteProcessingTime);
	list<thread> threads;
	running_writes = WriteThreadCount;
	for(uint i=0; i<ReadThreadCount; ++i) {
		threads.push_back(thread([=]{ read_thread(i); }));
	}
	for(uint i=0; i<WriteThreadCount; ++i) {
		threads.push_back(thread([=]{ write_thread(i); }));
	}
	auto start = steady_clock::now();
	for(thread &t : threads)
		t.join();
	auto end = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start);
	printf("%" PRIu64 " write ops, %" PRIu64 " read ops, %.3f million queue ops/sec\n", total_writes.load(), total_reads.load(), (double)total_writes.load() / (double)elapsed.count() * 1000.0 / 1000000.0);
	
	//getc(stdin);
	return 0;
}
