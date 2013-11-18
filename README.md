SpinPool
========

Thread Pool implementation with a spinning lock-free ring like the LMAX Disruptor just scales better.

Documentation
-------------

This project's goal is to scale much better than the Disruptor in one and only one case: when there are at most as many (or one less) threads as available cores, and almost all the threads are both readers and writers.  This allows executing packets in the smallest amount of time as there is no need to pass packets to other ring buffers or processing stages unless we have to execute them in parallel on multiple threads. It also prevents wasting of the hardware threads as in a complex graph you do not have to allocate a physical thread to a processing step.

As all the threads are equal (thread pool), there must be a mechanism which guarantees that one packet is only processed by only one thread. This mutual exclusion requires a CAS operation for reading. In order to guarantee that writer threads are not overwriting each other's slots, writing also requires a CAS operation. The program does not guarantee the processing order of packets other than every thread removes packets in order, but they do not wait until other threads are finished with other packets removed sooner. If logical order is necessary then the user of spinpool must enqueue queues of packets instead of the packets itself (this is not implemented yet, can degrade performance).

The idea is that there are no shared positions, every reader and writer has its own private read and write positions which are stored in that core's cache. The ring buffer holds 64 bit values which are made up of some bits of payload (currently not implemented) in the most significant bits, a counter and the least significant bit is whether the buffer position is empty or full. Since every position goes empty → full → empty → full and so on, the least significant bits create a monotonous counter. This monotony is used to determine whether the reader or writer can overwrite or read the current value or wait or skip it. In case we are behind the real current read or write positions then it tries to skip elements using an exponentially increasing skip count.

In order to reduce the effects of false sharing, the ring buffer is not continuous in memory but every next element resides in the next cache line, which is assumed to be 64 bytes. (See get_index(), unreming the simple return makes the ring buffer linear.) You can see that it wins around 50% to shuffle bits around if the threads are on different cores. In case of only 2 threads in the same SMT core it reduces performance but who cares?

Usage is (will be, when payload is implemented) simpler, the following patterns in the documentation can be implemented like this:

1. Unicast: no need for P1 → C1, everything must be executed synchronously, e.g. the thread which received the packet have to process it completely, passing it to other threads is a waste of time.

2. Thee step pipeline: see point 1., call the 3 processing steps one after another synchronously.

3. Sequencer 3P → 1C, the packet objects (which are stored in a table which is indexed by the payload) have to contain an atomic counter which starts from 3. The thread which decrements it to 0 has to do the rest of the processing synchronously.

4. Multicast 1P → 3C, you have to enqueue the packet two times and also process it synchronously (if it must be processed on all 3 threads), or enqueue once (if it must work as a thread pool) but this latter would be a waste of time, can be simply processed synchronously as we are already on a thread pool.

5. Diamond: combine Multicast and Sequencer.

