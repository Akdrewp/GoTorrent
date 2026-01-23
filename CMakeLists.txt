cmake_minimum_required(VERSION 3.20)
project(bittorrent_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. Find Dependencies ---
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED COMPONENTS system) 
find_package(Threads REQUIRED)

# --- 1.1 Test Depenency ---
find_package(GTest CONFIG REQUIRED)

# --- 2. Define Core Sources ---
set(CORE_SOURCES
    bencode/bencode.cpp
    torrent/torrent.cpp
    tracker/tracker.cpp
    client/client.cpp
    peer/peer.cpp
    peer/peerConnection.cpp
    client/torrentStorage/diskTorrentStorage.cpp
    client/torrentSession/torrentSession.cpp
    client/piecePicker/piecePicker.cpp
    client/pieceRepository/pieceRepository.cpp
)

# --- 3. Build Main Client ---
add_executable(bittorrent_client src/main.cpp ${CORE_SOURCES})

# Include Dirs
target_include_directories(bittorrent_client PRIVATE
    src bencode torrent tracker client peer
    client/torrentStorage client/torrentSession
    client/piecePicker client/pieceRepository
    client/chokingAlgorithm
)

target_link_libraries(bittorrent_client PRIVATE
    CURL::libcurl OpenSSL::SSL OpenSSL::Crypto
    spdlog::spdlog fmt::fmt Boost::system Threads::Threads
)

# --- 4. Build Tests ---
enable_testing()

# Define your test files
set(TEST_SOURCES
    bencode/test/bencode_test.cpp
    peer/test/peer_test.cpp
    client/torrentSession/test/torrentSession_test.cpp
    client/torrentStorage/test/diskTorrentStorage_test.cpp
    client/pieceRepository/test/pieceRepository_test.cpp
    client/piecePicker/test/piecePicker_test.cpp
)

# Create the Test Executable
add_executable(unit_tests ${TEST_SOURCES} ${CORE_SOURCES})

# Tests need the same Include Paths as the main app
target_include_directories(unit_tests PRIVATE
    src bencode torrent tracker client peer
    client/torrentStorage client/torrentSession
    client/piecePicker client/pieceRepository
    client/chokingAlgorithm
)

# Link Libraries + GTest
target_link_libraries(unit_tests PRIVATE
    GTest::gtest
    GTest::gtest_main  # This provides the "main()" function for tests
    GTest::gmock
    CURL::libcurl OpenSSL::SSL OpenSSL::Crypto
    spdlog::spdlog fmt::fmt Boost::system Threads::Threads
)

# --- 5. Register Tests ---
include(GoogleTest)
gtest_discover_tests(unit_tests)