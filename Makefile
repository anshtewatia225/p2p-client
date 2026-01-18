# P2P Group-Based File Sharing System
# Makefile for Windows (MinGW) and Linux

CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra

# Detect OS
ifeq ($(OS),Windows_NT)
    # Windows with MinGW
    LDFLAGS = -lws2_32
    TRACKER_EXE = tracker.exe
    CLIENT_EXE = client.exe
    RM = del /Q
else
    # Linux/Unix
    LDFLAGS = -pthread
    TRACKER_EXE = tracker
    CLIENT_EXE = client
    RM = rm -f
endif

all: $(TRACKER_EXE) $(CLIENT_EXE)

$(TRACKER_EXE): tracker.cpp
	$(CXX) $(CXXFLAGS) -o $(TRACKER_EXE) tracker.cpp $(LDFLAGS)

$(CLIENT_EXE): client.cpp
	$(CXX) $(CXXFLAGS) -o $(CLIENT_EXE) client.cpp $(LDFLAGS)

clean:
	$(RM) $(TRACKER_EXE) $(CLIENT_EXE)

.PHONY: all clean
