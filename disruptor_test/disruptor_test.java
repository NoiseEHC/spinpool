import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import com.lmax.disruptor.*;
import com.lmax.disruptor.dsl.Disruptor;
import com.lmax.disruptor.dsl.ProducerType;

public class disruptor_test {
	public static final class ValueEvent {
		private long value;

		public long getValue() {
			return value;
		}

		public void setValue(final long value) {
			this.value = value;
		}

		public final static EventFactory<ValueEvent> EVENT_FACTORY = new EventFactory<ValueEvent>() {
			public ValueEvent newInstance() {
				return new ValueEvent();
			}
		};
	}

	public static long Total;
	public static int ReadThreadCount;
	public static int WriteThreadCount;
	public static int ReadWriteThreadCount;
	public static RingBuffer<ValueEvent> ringBuffer;
	static volatile int alma = 0;

	public static class CounterHandler implements WorkHandler<ValueEvent> {
		long _padding0, _padding1, _padding2, _padding3, _padding4, _padding5,
				_padding6, _padding7, _padding8;
		public long handleCount = 0;
		long _paddingB0, _paddingB1, _paddingB2, _paddingB3, _paddingB4,
				_paddingB5, _paddingB6, _paddingB7, _paddingB8;

		public void onEvent(final ValueEvent event) throws Exception {
			// process a new event.
			++handleCount;
		}
	}

	public static class ReadWriteHandler extends CounterHandler {
		public void onEvent(final ValueEvent event) throws Exception {
			// process a new event.
			long sequence;
			switch ((int) (handleCount & 3)) {
			case 0:
				++handleCount;
				sequence = ringBuffer.next();
				ringBuffer.publish(sequence);
				sequence = ringBuffer.next();
				ringBuffer.publish(sequence);
				break;
			case 1:
				++handleCount;
				break;
			case 2:
				handleCount += 2;
				sequence = ringBuffer.next();
				ringBuffer.publish(sequence);
				break;
			// case 3 is not possible, we use & 3 as it is faster than % 3
			}
		}
	}

	public static class WriteThread implements Runnable {
		RingBuffer<ValueEvent> ringBuffer;

		public WriteThread(RingBuffer<ValueEvent> ringBuffer) {
			this.ringBuffer = ringBuffer;
		}

		public void run() {
			for (long x = 0; x < Total; x++) {
				// ARGH: putting it into a static function halves speed
				long sequence = ringBuffer.next();
				ringBuffer.publish(sequence);
			}
		}
	}

	public static void main(String[] args) {
		if (args.length != 4) {
			System.out
					.printf("Usage: disruptor_java <iterations in millions> <read thread count> <write thread count> <read/write thread count>\n");
			return;
		}
		Total = 1000000L * Long.parseLong(args[0]);
		ReadThreadCount = Integer.parseInt(args[1]);
		WriteThreadCount = Integer.parseInt(args[2]);
		ReadWriteThreadCount = Integer.parseInt(args[3]);

		System.out.printf(
				"starting with %d ops, read: %d, write: %d, read/write: %d\n",
				Total, ReadThreadCount, WriteThreadCount, ReadWriteThreadCount);

		ExecutorService readExecutor = Executors
				.newFixedThreadPool(ReadThreadCount + ReadWriteThreadCount);
		Disruptor<ValueEvent> disruptor = new Disruptor<ValueEvent>(
				ValueEvent.EVENT_FACTORY, 1024 * 1024, readExecutor,
				ProducerType.MULTI, new BusySpinWaitStrategy());
		CounterHandler[] handlers = new CounterHandler[ReadThreadCount
				+ ReadWriteThreadCount];
		for (int i = 0; i < ReadThreadCount; ++i)
			handlers[i] = new CounterHandler();
		for (int i = ReadThreadCount; i < ReadWriteThreadCount; ++i)
			handlers[i] = new ReadWriteHandler();
		disruptor.handleEventsWithWorkerPool(handlers);
		ringBuffer = disruptor.start();

		long start = System.currentTimeMillis();

		ExecutorService writeExecutor = null;
		if (WriteThreadCount != 0) {
			writeExecutor = Executors.newFixedThreadPool(WriteThreadCount);
			for (int i = 0; i < WriteThreadCount; ++i)
				writeExecutor.execute(new WriteThread(ringBuffer));
		}

		for (int i = 0; i < ReadWriteThreadCount; ++i) {
			long sequence = ringBuffer.next();
			ringBuffer.publish(sequence);
		}
		
		long expectedTotal = Total*WriteThreadCount + Total*ReadWriteThreadCount*3/4;
		while (true) {
			++alma; // memory barrier hack
			long handleCount = 0;
			for (int i = 0; i < handlers.length; ++i)
				handleCount += handlers[i].handleCount;
			if (handleCount >= expectedTotal)
				break;
		}

		double opsPerSecond = (expectedTotal / 1000.0)
				/ (System.currentTimeMillis() - start);
		System.out.printf("%d write ops, %.3f million ops/sec\n", expectedTotal,
				opsPerSecond);

		disruptor.shutdown();
		readExecutor.shutdown();
		if(writeExecutor != null)
			writeExecutor.shutdown();
	}
}