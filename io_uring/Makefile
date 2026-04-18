CXX      := g++
CXXFLAGS := -std=c++20 -Wall -Wextra -g
LDFLAGS  := -luring

SRCS := $(wildcard *.cpp)
BINS := $(SRCS:.cpp=)

all: $(BINS)

%: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Boost.Asio samples need extra flags
16_%: LDFLAGS += -lpthread
16_%: CXXFLAGS += -DBOOST_ASIO_HAS_IO_URING

clean:
	rm -f $(BINS)

.PHONY: all clean
