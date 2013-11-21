CC         = g++
CFLAGS     = -O3 -c -Wall -std=c++11 -g
LDFLAGS    = -pthread

SOURCES    = atomic_queue.cpp
OBJECTS    = $(SOURCES:.cpp=.o)

EXECUTABLE = atomic_queue

all: $(OBJECTS) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm $(OBJECTS) $(EXECUTABLE)
