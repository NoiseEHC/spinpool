#include <thread>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <xmmintrin.h>
using namespace std;
using namespace std::chrono;
//typedef unsigned long long ulong;

static const ulong size = 1 << 20;
static const ulong mask = size-1;
static const ulong iterations = 1000000000;
static volatile ulong buffer[size] = {};

int main(int argc, char* argv[])
{
	auto start = steady_clock::now();
	thread reader([] {
		for(ulong i=0; i<iterations; ++i) {
			ulong index = i&mask;

			//volatile ulong *p = buffer+index;
			//ulong value;
			//while((value = *p) == 0)
			//	_mm_pause();
			//if(value != i+5)
			//	abort();
			//*p = 0;

			auto p = (atomic<ulong> *)buffer+index;
			ulong value;
			while((value = p->load(memory_order_acquire)) == 0)
				_mm_pause();
			if(value != i+5)
				abort();
			p->store(0, memory_order_relaxed);
		}
	});
	thread writer([] {
		for(ulong i=0; i<iterations; ++i) {
			ulong index = i&mask;
			
			//volatile ulong *p = buffer+index;
			//while(*p != 0)
			//	_mm_pause();
			//*p = i+5;
			
			auto p = (atomic<ulong> *)buffer+index;
			while(p->load(memory_order_relaxed) != 0)
				_mm_pause();
			p->store(i+5, memory_order_release);
		}
	});
	writer.join();
	reader.join();
	auto end = steady_clock::now();
    auto elapsed = duration_cast<milliseconds>(end - start);
	cout << fixed << setprecision(3) << iterations / 1000.0 / elapsed.count() << " million ops/sec" << endl;
	return 0;
}
