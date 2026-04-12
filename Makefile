CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g
LDFLAGS  := -luring

SRCS := $(wildcard *.cpp)
BINS := $(SRCS:.cpp=)

all: $(BINS)

%: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BINS)

.PHONY: all clean
