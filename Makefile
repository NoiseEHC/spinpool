CC         = g++
CFLAGS     = -O3 -c -Wall -std=c++11
LDFLAGS    = -pthread

SOURCES    = spinpool.cpp
OBJECTS    = $(SOURCES:.cpp=.o)

EXECUTABLE = spinpool

all: $(OBJECTS) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

.cpp.o:
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm $(OBJECTS) $(EXECUTABLE)

