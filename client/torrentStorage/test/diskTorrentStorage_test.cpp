#include <gtest/gtest.h>
#include "bencode.h"
#include "diskTorrentStorage.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <memory>

namespace fs = std::filesystem;
using namespace testing;

class DiskTorrentStorageTest : public Test {
protected:
  std::string testDir = "test_storage_downloads";
  std::string testFilename = "storage_test.bin";
  std::shared_ptr<DiskTorrentStorage> storage;

  void SetUp() override {
    // Delete existing test directory
    if (fs::exists(testDir)) {
      fs::remove_all(testDir);
    }
    storage = std::make_shared<DiskTorrentStorage>();
  }

  void TearDown() override {
    // Delete testing directory
    if (fs::exists(testDir)) {
      fs::remove_all(testDir);
    }
  }

  // Helper to create dummy TorrentData with just the info needed for storage
  TorrentData createDummySingleFileTorrent(const std::string& name, long long length) {
    TorrentData t;
    BencodeDict info;
    
    // Need name and length for single file torrents
    info["name"] = makeBencode(name);
    info["length"] = makeBencode(length);

    t.mainData["info"] = makeBencode(std::move(info));
    return t;
  }
};

TEST_F(DiskTorrentStorageTest, singleFileTorrentInitialize_ShouldCreateDirectoryAndFile_WhenDirectoryAndFileDoNotExists) {
  // Create single file torrent
  TorrentData torrent = createDummySingleFileTorrent(testFilename, 1024 * 1024);
  long long pieceLength = 16384; // 16KB

  // Initiailize
  storage->initialize(torrent, pieceLength, testDir);

 
  fs::path dirPath(testDir);
  fs::path filePath = dirPath / testFilename;

  // Expectation: The directory is created since it doesn't exists
  EXPECT_TRUE(fs::exists(dirPath)) << "Directory should be created";

  // Expectation: The file is created with the correct name since it doesn't exist
  EXPECT_TRUE(fs::exists(filePath)) << "File should be created";
}

TEST_F(DiskTorrentStorageTest, SingleFileTorrentInitialize_ShouldThrowError_WhenFileAlreadyExists) {
  // Create the file manually to simulate pre-existing file
  fs::create_directories(testDir);
  fs::path filePath = fs::path(testDir) / testFilename;
  std::ofstream outfile(filePath);
  outfile.close();

  ASSERT_TRUE(fs::exists(filePath));

  // Create single file torrent with same name
  TorrentData torrent = createDummySingleFileTorrent(testFilename, 1024 * 1024);
  long long pieceLength = 16384;

  // Expectation: Should throw runtime_error because the file already exists
  EXPECT_THROW({
    storage->initialize(torrent, pieceLength, testDir);
  }, std::runtime_error);
}

// writePiece

TEST_F(DiskTorrentStorageTest, WritePiece_ShouldWriteDataAtCorrectOffset) {
  // Create single file torrent
  TorrentData torrent = createDummySingleFileTorrent(testFilename, 1024 * 1024);
  long long pieceLength = 10; // Small size for easy testing
  
  // Initiailize
  storage->initialize(torrent, pieceLength, testDir);

  // Piece 0 Data: 0-9
  std::vector<uint8_t> data0(10, 'A'); 
  storage->writePiece(0, data0);

  // Piece 2 Data: 20-29
  std::vector<uint8_t> data2(10, 'C'); 
  storage->writePiece(2, data2);

  // Verify by reading the file back
  fs::path filePath = fs::path(testDir) / testFilename;
  std::ifstream ifs(filePath, std::ios::binary);
  ASSERT_TRUE(ifs.is_open());

  // Expectation: File should be 30 bytes
  ifs.seekg(0, std::ios::end);
  EXPECT_GE(ifs.tellg(), 30);
  ifs.seekg(0, std::ios::beg);

  std::vector<char> buffer(30);
  ifs.read(buffer.data(), 30);

  // Expectation: Offset 0-9 corresponding to piece 0 should be all 'A'
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(buffer[i], 'A') << "Mismatch at index " << i;
  }

  // Expectation: Offset 10-19 Should be empty since piece 1 was never written
  for (int i = 10; i < 20; ++i) {
    EXPECT_EQ(buffer[i], 0) << "Gap should be 0 (Empty) at index " << i;
  }

  // Expectation: Offset 20-29 corresponding to piece C should be all 'C'
  for (int i = 20; i < 30; ++i) {
    EXPECT_EQ(buffer[i], 'C') << "Mismatch at index " << i;
  }
}

TEST_F(DiskTorrentStorageTest, WritePiece_ShouldThrowError_IfStorageNotInitialized) {
  std::vector<uint8_t> data(10, 'A');
  
  // Expectation: Write piece throws an error when storage is not initialized
  EXPECT_THROW({
    storage->writePiece(0, data);
  }, std::runtime_error);
}

TEST_F(DiskTorrentStorageTest, WritePiece_ShouldOverwriteExistingData_IfCalledOnTheSameOffsetTwice) {
  // Create single file torrent
  TorrentData torrent = createDummySingleFileTorrent(testFilename, 1024 * 1024);
  long long pieceLength = 10; // Small size for easy testing
  
  // Initiailize
  storage->initialize(torrent, pieceLength, testDir);

  // Write Bad Data all 'X"
  std::vector<uint8_t> badData(10, 'X');
  storage->writePiece(0, badData);

  // Write Good Data all 'A'
  std::vector<uint8_t> goodData(10, 'A');
  storage->writePiece(0, goodData);

  // Expecation: Data at 0-9 should be all 'A' after overwriting the all 'X'
  fs::path filePath = fs::path(testDir) / testFilename;
  std::ifstream ifs(filePath, std::ios::binary);
  std::vector<char> buffer(10);
  ifs.read(buffer.data(), 10);

  for (char c : buffer) {
    EXPECT_EQ(c, 'A');
  }
}

// readPiece

TEST_F(DiskTorrentStorageTest, ReadBlock_ShouldReturnAllAs_WhenCalledOnPieceWrittenWithAllAs) {
  // Create single file torrent
  TorrentData torrent = createDummySingleFileTorrent(testFilename, 1024);
  long long pieceLength = 10;
  
  // Initialize
  storage->initialize(torrent, pieceLength, testDir);

  // Write Data all 'A' to Piece 0
  std::vector<uint8_t> expectedData(10, 'A');
  storage->writePiece(0, expectedData);

  // Read back the piece (using readBlock for the full piece length)
  std::vector<uint8_t> readData = storage->readBlock(0, 0, 10);

  // Expectation: Data read back matches data written
  EXPECT_EQ(readData.size(), 10);
  for (size_t i = 0; i < readData.size(); ++i) {
    EXPECT_EQ(readData[i], 'A') << "Mismatch at index " << i;
  }
}

TEST_F(DiskTorrentStorageTest, ReadBlock_ShouldReturnCorrectData_WhenReadingSecondPiece) {
  // Create single file torrent
  TorrentData torrent = createDummySingleFileTorrent(testFilename, 1024);
  long long pieceLength = 10;
  
  // Initialize
  storage->initialize(torrent, pieceLength, testDir);

  // Write Data all 'B' to Piece 1 (Offset 10-19)
  std::vector<uint8_t> expectedData(10, 'B');
  storage->writePiece(1, expectedData);

  // Read back Piece 1
  std::vector<uint8_t> readData = storage->readBlock(1, 0, 10);

  // Expectation: Data read back matches data written
  EXPECT_EQ(readData.size(), 10);
  for (size_t i = 0; i < readData.size(); ++i) {
    EXPECT_EQ(readData[i], 'B') << "Mismatch at index " << i;
  }
}

TEST_F(DiskTorrentStorageTest, ReadBlock_ShouldReturnZeros_WhenReadingUnwrittenData) {
  // Create single file torrent
  TorrentData torrent = createDummySingleFileTorrent(testFilename, 1024);
  long long pieceLength = 10;
  
  // Initialize
  storage->initialize(torrent, pieceLength, testDir);

  // Read back Piece 0 without writing anything
  std::vector<uint8_t> readData = storage->readBlock(0, 0, 10);

  // Expectation: Should return 0s (sparse/pre-allocated)
  EXPECT_EQ(readData.size(), 10);
  for (size_t i = 0; i < readData.size(); ++i) {
    EXPECT_EQ(readData[i], 0) << "Mismatch at index " << i;
  }
}

TEST_F(DiskTorrentStorageTest, ReadBlock_ShouldThrowError_WhenReadingPastEndOfFile) {
  // Create single file torrent, Length 20 bytes
  TorrentData torrent = createDummySingleFileTorrent(testFilename, 20);
  long long pieceLength = 10;
  
  // Initialize
  storage->initialize(torrent, pieceLength, testDir);

  // Expectation: An error is thrown since 10 bytes after piece 2 is EOF
  // Piece 2 (Offset 20, Length 10) = Past EOF
  EXPECT_THROW({
    storage->readBlock(2, 0, 10);
  }, std::runtime_error);
}