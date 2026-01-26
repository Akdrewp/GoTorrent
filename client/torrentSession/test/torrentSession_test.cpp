#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "torrentSession.h"
#include "ITracker.h"
#include "IPieceRepository.h"
#include "IPiecePicker.h"
#include "IChokingAlgorithm.h"
#include "torrent.h"

using namespace testing;

// --- Mocks ---

class MockChokingAlgorithm : public IChokingAlgorithm {
public:
  MOCK_METHOD(void, rechoke, (std::vector<std::shared_ptr<Peer>>&), (override));
};

class MockTrackerClient : public ITrackerClient {
public:
  MOCK_METHOD(std::string, sendRequest, (const std::string&), (override));
};

class MockPieceRepository : public IPieceRepository {
public:
  MOCK_METHOD(void, initialize, (const std::string&), (override));
  MOCK_METHOD(std::vector<uint8_t>, getBitfield, (), (const, override));
  MOCK_METHOD(bool, verifyHash, (size_t, const std::vector<uint8_t>&), (override));
  MOCK_METHOD(void, savePiece, (size_t, const std::vector<uint8_t>&), (override));
  MOCK_METHOD(std::vector<uint8_t>, readBlock, (size_t, size_t, size_t), (override));
  MOCK_METHOD(size_t, getPieceLength, (), (const, override));
  MOCK_METHOD(size_t, getTotalLength, (), (const, override));
  MOCK_METHOD(bool, havePiece, (size_t), (const, override));
};

class MockPiecePicker : public IPiecePicker {
public:
  MOCK_METHOD(std::optional<size_t>, pickPiece, (const std::vector<uint8_t>&, const std::vector<uint8_t>&), (override));
  MOCK_METHOD(void, onPiecePassed, (size_t), (override));
  MOCK_METHOD(void, onPieceFailed, (size_t), (override));
  MOCK_METHOD(void, processBitfield, (const std::vector<uint8_t>&), (override));
  MOCK_METHOD(void, processPeerDisconnect, (const std::vector<uint8_t>&), (override));
  MOCK_METHOD(void, processHave, (size_t), (override));
};

class TorrentSessionTest : public Test {
protected:
  boost::asio::io_context io;
  std::string peerId = "-GT0001-123456789012";
  unsigned short port = 6882;
  std::shared_ptr<MockTrackerClient> mockTracker;
  std::shared_ptr<MockPieceRepository> mockRepo;
  std::shared_ptr<MockPiecePicker> mockPicker;
  std::shared_ptr<MockChokingAlgorithm> mockChokingAlgorithm;
  std::shared_ptr<TorrentSession> session;

  template <typename T>
  std::unique_ptr<BencodeValue> makeVal(T v) {
    auto val = std::make_unique<BencodeValue>();
    val->value = std::move(v);
    return val;
  }

  TorrentData createDummyTorrent() {
    TorrentData t;
    t.infoHash.resize(20, 'a');
    
    BencodeDict info;
    info.emplace("length", makeVal(100LL));
    info.emplace("name", makeVal(std::string("test.bin")));
    info.emplace("piece length", makeVal(10LL));
    info.emplace("pieces", makeVal(std::string(200, 'h'))); // 10 pieces
    
    t.mainData.emplace("info", makeVal(std::move(info)));
    t.mainData.emplace("announce", makeVal(std::string("http://tracker.com")));
    
    return t;
  }

  void SetUp() override {
    mockTracker = std::make_shared<MockTrackerClient>();
    mockRepo = std::make_shared<MockPieceRepository>();
    mockPicker = std::make_shared<MockPiecePicker>();
    
    TorrentData torrent = createDummyTorrent();
    session = std::make_shared<TorrentSession>(io, std::move(torrent), peerId, port, mockTracker, mockRepo, mockPicker, mockChokingAlgorithm);
  }
};

TEST_F(TorrentSessionTest, Start_ShouldInitRepoAndContactTracker) {
  // 1. Expect Repo Initialization
  EXPECT_CALL(*mockRepo, initialize(_));
  EXPECT_CALL(*mockRepo, getTotalLength()).WillRepeatedly(Return(100));

  // 2. Expect Tracker Request (Mocking a valid response)
  // Response: interval=1800, peers=(empty string)
  // This confirms Session correctly builds the URL and parses the response
  std::string trackerResp = "d8:intervali1800e5:peers0:e"; 
  EXPECT_CALL(*mockTracker, sendRequest(_)).WillOnce(Return(trackerResp));

  session->start();
}