# Makefile for BitTorrent Client
# Place this in the *root* directory (the one containing src/, bencode/, etc.)
#
# To use:
# - `make`: Builds the main bittorrent_client
# - `make client`: Same as `make`
# - `make tests`: Builds the test runner
# - `make run_tests`: Builds AND runs your tests
# - `make clean`: Removes all built files

# 1. Compiler and Flags
CXX = g++
CXXFLAGS = -g -Wall -std=c++17

# 2. Libraries
# Add libraries for the client
# OpenSSL
# cURL
# boost
LIBS = -lcrypto -lcurl -lspdlog -lboost_system
# Add libraries for testing
# GoogleTest
# GoogleMock
TEST_LIBS = -lgtest -lgtest_main -lgmock -pthread

# 3. Project Structure
# Add all directories that contain .h files
INC_DIRS = -Isrc \
           -Ibencode \
           -Itorrent \
           -Itracker \
           -Iclient \
           -Ipeer \
           -Iclient/torrentStorage \
           -Iclient/torrentSession
CXXFLAGS += $(INC_DIRS)

# 4. Targets
CLIENT_TARGET = bittorrent_client
TEST_TARGET = test_runner_executable

# 5. Find Source Files Automatically
# Find all .cpp files in these directories for the CLIENT
# /src
# /bencode
# /torrent
# /tracker
# /client
# /peer
CLIENT_SRCS = $(wildcard src/*.cpp) \
              $(wildcard bencode/*.cpp) \
              $(wildcard torrent/*.cpp) \
              $(wildcard tracker/*.cpp) \
              $(wildcard client/*.cpp) \
              $(wildcard peer/*.cpp) \
              $(wildcard client/torrentStorage/*.cpp) \
              $(wildcard client/torrentSession/*.cpp)
CLIENT_OBJS = $(CLIENT_SRCS:.cpp=.o)

# Define source files for the TESTS
TEST_SRCS = bencode/test/bencode_test.cpp \
            bencode/bencode.cpp \
            peer/test/peer_test.cpp \
            client/torrentSession/test/torrentSession_test.cpp \
            client/torrentStorage/test/diskTorrentStorage_test.cpp \
            peer/peer.cpp \
            peer/peerConnection.cpp \
            client/torrentSession/torrentSession.cpp \
            tracker/tracker.cpp \
            torrent/torrent.cpp \
            client/torrentStorage/diskTorrentStorage.cpp

TEST_OBJS = $(TEST_SRCS:.cpp=.o)

# 6. Build Rules

# Default rule (runs when you just type `make`)
all: client

# Rule to build the main client
client: $(CLIENT_TARGET)

$(CLIENT_TARGET): $(CLIENT_OBJS)
	@echo "Linking client: $(CLIENT_TARGET)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: ./$(CLIENT_TARGET)"

# Rule to build the test runner executable
tests: $(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJS)
	@echo "Linking tests: $(TEST_TARGET)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(TEST_LIBS) $(LIBS)
	@echo "Test build complete: ./$(TEST_TARGET)"

# Rule to run the tests (depends on the 'tests' build rule)
run_tests: tests
	@echo "Running tests..."
	./$(TEST_TARGET)

# Generic rule to compile any .cpp file into a .o (object) file
# $< means the first prerequisite (the .cpp file)
# $@ means the target (the .o file)
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to clean up
clean:
	@echo "Cleaning up..."
	rm -f $(CLIENT_TARGET) $(TEST_TARGET) $(CLIENT_OBJS) $(TEST_OBJS)

