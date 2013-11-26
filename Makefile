CC         = g++
CFLAGS     = -O3 -c -Wall -std=c++11 -g
LDFLAGS    = -pthread

SOURCES    = spinpool.cpp
HEADERS    = spinallocator.hpp spinlock.hpp
OBJECTS    = $(SOURCES:.cpp=.o)

EXECUTABLE = spinpool

all: $(OBJECTS) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

.cpp.o: $(HEADERS)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm $(OBJECTS) $(EXECUTABLE)

