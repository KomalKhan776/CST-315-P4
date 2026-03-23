CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g
TARGET   = lopeshell

all: $(TARGET)

$(TARGET): lopeshell.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGET) /tmp/lope_swap.bin

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
