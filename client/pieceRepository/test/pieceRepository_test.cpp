#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "pieceRepository.h" 
#include "ITorrentStorage.h"
#include "torrent.h"
#include <openssl/sha.h>

using namespace testing;

// --- Mocks ---

class MockTorrentStorage : public ITorrentStorage {
public:
  MOCK_METHOD(void, initialize, (const TorrentData&, long long, const std::string&), (override));
  MOCK_METHOD(void, writePiece, (size_t, const std::vector<uint8_t>&), (override));
  MOCK_METHOD(std::vector<uint8_t>, readBlock, (size_t, size_t, size_t), (override));
};

// --- Test Fixture ---

class PieceRepositoryTest : public Test {
protected:
  std::shared_ptr<MockTorrentStorage> mockStorage;
  std::shared_ptr<PieceRepository> repo;
  TorrentData torrent;

  // Helper to create Bencode Values for constructing TorrentData
  template <typename T>
  std::unique_ptr<BencodeValue> makeVal(T v) {
    auto val = std::make_unique<BencodeValue>();
    val->value = std::move(v);
    return val;
  }

  void SetUp() override {
    mockStorage = std::make_shared<MockTorrentStorage>();
  }

  /**
   * @brief Helper to initialize the repo with specific piece hashes
   */
  void InitializeRepo(const std::string& piecesStr, long long pieceLength) {
    // Construct minimal Bencoded Info Dictionary
    BencodeDict info;
    info.emplace("piece length", makeVal(pieceLength));
    info.emplace("pieces", makeVal(piecesStr));
    
    // Add length/name to satisfy potential internal checks
    info.emplace("length", makeVal(1024LL)); 
    info.emplace("name", makeVal(std::string("test.bin")));

    torrent.mainData.emplace("info", makeVal(std::move(info)));

    // Create Repo
    repo = std::make_shared<PieceRepository>(mockStorage, torrent);
    
    // Expect storage init call
    EXPECT_CALL(*mockStorage, initialize(_, _, _));
    repo->initialize("./downloads");
  }
};

// --- Tests ---

TEST_F(PieceRepositoryTest, VerifyHash_ShouldReturnTrue_WhenHashIsValid) {
  // Create dummy data for the piece
  std::string content = "This is a test piece data string.";
  std::vector<uint8_t> data(content.begin(), content.end());

  // Initialize repo with correct hash
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(data.data(), data.size(), hash);
  std::string piecesStr(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);
  InitializeRepo(piecesStr, static_cast<long long>(data.size()));

  // Expectation: Verify hash returns true when data with correct hash is passed
  EXPECT_TRUE(repo->verifyHash(0, data));
}

TEST_F(PieceRepositoryTest, VerifyHash_ShouldReturnFalse_WhenHashIsInvalid) {
  // Create dummy data for the piece
  std::string content = "Correct Content";
  std::vector<uint8_t> correctData(content.begin(), content.end());

  // Initialize repo with correct hash
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(correctData.data(), correctData.size(), hash);
  std::string piecesStr(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);
  InitializeRepo(piecesStr, static_cast<long long>(correctData.size()));

  // Create bad data different from initial data
  std::string badContent = "Corrupt Content";
  std::vector<uint8_t> badData(badContent.begin(), badContent.end());

  // Expectation: Verify hash returns false when data with incorrect hash is passed
  EXPECT_FALSE(repo->verifyHash(0, badData));
}

TEST_F(PieceRepositoryTest, SavePiece_ShouldWriteToStorageAndUpdateBitfield) {
  // Create dummy data for the piece
  std::string content = "Saved Data";
  std::vector<uint8_t> data(content.begin(), content.end());
  
  // Initialize repo with correct hash
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(data.data(), data.size(), hash);
  std::string piecesStr(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);
  InitializeRepo(piecesStr, static_cast<long long>(data.size()));

  const size_t PIECE_IDX = 0;

  // Expectation: Bitfield should be update to NOT have piece at PIECE_IDX
  EXPECT_FALSE(repo->havePiece(PIECE_IDX));

  // Expectation: Storage writePiece is called with correct index and data
  EXPECT_CALL(*mockStorage, writePiece(PIECE_IDX, data));

  // Save Piece
  repo->savePiece(PIECE_IDX, data);

  // Expectation: Bitfield should be update to have piece at PIECE_IDX
  EXPECT_TRUE(repo->havePiece(PIECE_IDX));
}

TEST_F(PieceRepositoryTest, ReadBlock_ShouldReturnData_WhenPieceIsPresent) {
  // Create dummy data for the piece
  std::string content = "Full Piece Data Block Content";
  std::vector<uint8_t> data(content.begin(), content.end());
  
  // Initialize repo with correct hash
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(data.data(), data.size(), hash);
  std::string piecesStr(reinterpret_cast<char*>(hash), SHA_DIGEST_LENGTH);
  InitializeRepo(piecesStr, static_cast<long long>(data.size()));
  
  const size_t PIECE_IDX = 0;
  
  // Save piece at PIECE_IDX
  EXPECT_CALL(*mockStorage, writePiece(PIECE_IDX, _)); 
  repo->savePiece(PIECE_IDX, data);
  ASSERT_TRUE(repo->havePiece(PIECE_IDX));

  // We want to read block 5 from offset 5
  size_t offset = 5;
  size_t length = 5;
  std::vector<uint8_t> expectedBlock(data.begin() + offset, data.begin() + offset + length); 

  // Expectation: readBlock is called with requesite data
  EXPECT_CALL(*mockStorage, readBlock(PIECE_IDX, offset, length))
      .WillOnce(Return(expectedBlock));
  std::vector<uint8_t> result = repo->readBlock(PIECE_IDX, offset, length);

  // Expectation: The expected block is returned from readBlock
  EXPECT_EQ(result, expectedBlock);
}

TEST_F(PieceRepositoryTest, ReadBlock_ShouldThrow_WhenPieceIsNotPresent) {
  // Create dummy data for the piece
  std::string dummyHash(20, 'a');
  InitializeRepo(dummyHash, 100);
  
  const size_t PIECE_IDX = 0;

  // Ensure we don't have it (Bitfield not set)
  ASSERT_FALSE(repo->havePiece(PIECE_IDX));

  // Expectation: Storage should NOT be called
  EXPECT_CALL(*mockStorage, readBlock(_, _, _)).Times(0);

  // Expectation: readBlock throws an error when trying to read
  // block 10 which hasn't been written to
  EXPECT_THROW(repo->readBlock(PIECE_IDX, 0, 10), std::runtime_error);
}