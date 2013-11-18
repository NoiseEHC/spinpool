//             Copyright Andrew Rafas 2013-2013.
// Distributed under the Boost Software License, Version 1.0.
//     (See accompanying file LICENSE_1_0.txt or copy at
//           http://www.boost.org/LICENSE_1_0.txt)

#if defined(_MSC_VER)
#include <Windows.h>
typedef unsigned long long ulong;
typedef unsigned int uint;
#define PRIu64 "I64u"
#endif

#include <list>
#include <atomic>
#include <thread>
#include <cassert>

#if defined(__GNUC__) || defined(__GNUG__)
#include <xmmintrin.h>
#include <stdio.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#define _InterlockedCompareExchange64(p,n,o) __sync_val_compare_and_swap(p,o,n)
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
const unsigned int RingBits = 20; // 1M
const ulong RingSize = ((ulong)1) << RingBits;
const ulong RingMask = RingSize-1;
ulong Total = 0;
volatile ulong Ring[RingSize] = {};
const unsigned int CacheLineSizeBits = 3; // 2**3 * 8 byte = 64 byte
const ulong CacheLineSizeMask = ((((ulong)1) << CacheLineSizeBits)-1) << (RingBits-CacheLineSizeBits);

atomic<bool> thread_start_sync(false);
atomic<ulong> total_writes;

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

class Reader {
private:
	char _padding1[64];
	ulong _position;
	volatile ulong *_ring;

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

	Reader(volatile ulong *ring) {
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

			ulong value = _ring[index];
		try_again:
			assert(value >= expected_full-2);

			if(value < expected_full) {
				++retry1;
				_mm_pause();
				continue;
			}

			if(value == expected_full) {
				value = _InterlockedCompareExchange64((volatile long long *)&_ring[index], value+1, expected_full);
				if(value == expected_full) {
					++_position;
					return value;
				}
				++retry2;
				goto try_again;
			} else { // skip or we are behind
				ulong skip_size = 1;
				while(true) {
					ulong test_position = _position+skip_size*2;
					index = ::get_index(test_position);
					expected_full = get_expected_full(test_position);
					if(_ring[index] < expected_full+2)
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
	volatile ulong *_ring;

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

	Writer(volatile ulong *ring) {
		_position = 2*RingSize;
		_ring = ring;
		retry1 = 0;
		retry2 = 0;
		multiskip = 0;
	}

	void write() {
		while(true) {
			ulong index = get_index();
			//_m_prefetch(((char*)&_ring[index])+64);
			ulong expected_empty = get_expected_empty();

			ulong value = _ring[index];
		try_again:
			assert(value >= expected_empty-1);

			if(value < expected_empty) {
				++retry1;
				_mm_pause();
				continue;
			}

			if(value == expected_empty) {
				value = _InterlockedCompareExchange64((volatile long long *)&_ring[index], value+1, expected_empty);
				if(value == expected_empty) {
					//printf("W:%d ", index);
					++_position;
					return;
				}
				++retry2;
				goto try_again;
			} else { // skip or we are behind
				ulong skip_size = 1;
				while(true) {
					ulong test_position = _position+skip_size*2;
					index = ::get_index(test_position);
					expected_empty = get_expected_empty(test_position);
					if(_ring[index] < expected_empty+2)
						break;
					skip_size *= 2;
					++multiskip;
				}
				_position += skip_size;
			}
		}
	}
};

void read_thread() {
	Reader r(Ring);
	ulong success = 0;
	wait_for_thread_start();
	while(true) {
		ulong value = r.read();
		if(value == ((Total >> (RingBits-1)) | 1)+4)
			break;
		++success;
		wait_pause(ProcessingTime);
	}
	printf("Read: %.3f (%" PRIu64 "), Retry: %.3f %.3f, Multiskip: %" PRIu64 "\n", success/1000000.0, success, r.retry1/1000000.0, r.retry2/1000000.0, r.multiskip);
}

void write_thread(int index) {
#if defined(_MSC_VER)
	printf("Old affinity for %d: %d\n", index, SetThreadAffinityMask(GetCurrentThread(), 1<<index));
#endif
	Writer w(Ring);
	wait_for_thread_start();
	ulong success = 0;
	ulong total = Total/WriteThreadCount+1;
	for(ulong count = 0; count < total; ++count) {
		w.write();
		++success;
	}
	printf("Written: %.3f (%" PRIu64 ") Retry: %.3f %.3f, Multiskip: %" PRIu64 "\n", success/1000000.0, success, w.retry1/1000000.0, w.retry2/1000000.0, w.multiskip);
	total_writes += success;
}

void read_write_thread(int index) {
#if defined(_MSC_VER)
	printf("Old affinity for %d: %d\n", index, SetThreadAffinityMask(GetCurrentThread(), 1<<index));
#endif
	Reader r(Ring);
	Writer w(Ring);
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

	for(unsigned int i=0; i<RingSize; ++i) {
		Ring[i] = 4; // starting with 0 causes the asserts to fail because of the underflow of unsigned values
	}
	printf("starting with %" PRIu64 " ops, read: %d, write: %d, read/write: %d, pause loops: %d\n", Total, ReadThreadCount, WriteThreadCount, ReadWriteThreadCount, ProcessingTime);
	list<thread> threads;
	for(unsigned int i=0; i<ReadThreadCount; ++i) {
		threads.push_back(thread(read_thread));
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
