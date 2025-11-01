# This Makefile builds bencode related files
#
# To use:
# - `make client`: Builds your main bencode_client
# - `make tests`: Builds your test runner
# - `make run_tests`: Builds AND runs your tests
# - `make clean`: Removes all built files

# 1. Compiler and Flags
CXX = g++
CXXFLAGS = -g -Wall -std=c++17 -I. # -I. tells C++ to look in the current dir

# 2. Project Structure
# Define the directory for your bencode library
BCODE_DIR = bencode
# Add the bencode directory to the compiler's include path
CXXFLAGS += -I$(BCODE_DIR)

# Tell 'make' where to find source files
# It will look in the current directory ('.') AND in BCODE_DIR
vpath %.cpp .:$(BCODE_DIR)
vpath %.h .:$(BCODE_DIR)

# 3. Test Flags
GTEST_INCLUDE = /usr/include
GTEST_LIBS = -lgtest -lpthread

# 4. Our Source Files
# List the *source file names*. 'make' will find them using vpath.
CLIENT_SRCS = main.cpp bencode.cpp
TEST_SRCS = bencode_test.cpp bencode.cpp

# List the *object file names*.
# These will be created in the root directory.
CLIENT_OBJS = $(CLIENT_SRCS:.cpp=.o)
TEST_OBJS = $(TEST_SRCS:.cpp=.o)

# 5. Target Names
CLIENT_TARGET = bittorrent_client
TEST_TARGET = test_runner_executable

# 6. Build Rules

# This is the default rule (runs when you just type `make`)
all: client

# Rule to build the main client
client: $(CLIENT_TARGET)

$(CLIENT_TARGET): $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) -o $(CLIENT_TARGET) $(CLIENT_OBJS)

# Rule to build the test runner executable
tests: $(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) -o $(TEST_TARGET) $(TEST_OBJS) -I$(GTEST_INCLUDE) $(GTEST_LIBS)

# Rule to run the tests (depends on the 'tests' build rule)
run_tests: tests
	./$(TEST_TARGET)

# Generic rule to compile a .cpp file into a .o (object) file
# This rule will now use 'vpath' to find the .cpp file
# (e.g., it will find 'bencode.cpp' in the 'bencode/' dir)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to clean up
clean:
	rm -f $(CLIENT_TARGET) $(TEST_TARGET) *.o

