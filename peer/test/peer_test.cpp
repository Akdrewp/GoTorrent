#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "peer.h"
#include "ITorrentSession.h"
#include "IPieceRepository.h"
#include "IPiecePicker.h"
#include "peerConnection.h"
#include <cstring>
#include <arpa/inet.h> // For ntohl
#include <openssl/sha.h>

using namespace testing;

// --- Custom Matcher to verify Binary Payload ---
MATCHER_P3(HasRequestPayload, index, begin, length, "") {
  if (arg.size() != 12) return false;

  uint32_t actualIndex, actualBegin, actualLength;
  std::memcpy(&actualIndex, arg.data(), 4);
  std::memcpy(&actualBegin, arg.data() + 4, 4);
  std::memcpy(&actualLength, arg.data() + 8, 4);

  // Convert back from Network Byte Order to Host
  actualIndex = ntohl(actualIndex);
  actualBegin = ntohl(actualBegin);
  actualLength = ntohl(actualLength);

  return actualIndex == index && actualBegin == begin && actualLength == length;
}

// --- Mocks ---

class MockTorrentSession : public ITorrentSession {
public:
  // ITorrentSession is mostly empty now, used mainly for lifetime checks (weak_ptr)
  MOCK_METHOD(void, onPeerDisconnected, (std::shared_ptr<Peer> peer), (override));
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

// Mock Connection
class MockPeerConnection : public PeerConnection {
public:
  // Pass dummy io_context to base constructor
  MockPeerConnection(boost::asio::io_context& io) : PeerConnection(io, "127.0.0.1", 0) {}

  MOCK_METHOD(void, sendMessage, (uint8_t id, const std::vector<unsigned char>& payload), (override));
  MOCK_METHOD(void, close, (const boost::system::error_code&), (override));
  
  // Helper to trigger callbacks manually
  HandshakeCallback storedHandshakeHandler;
  MessageCallback storedMessageHandler;

  void startAsOutbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    HandshakeCallback handshakeHandler,
    MessageCallback messageHandler
  ) override {
    storedHandshakeHandler = handshakeHandler;
    storedMessageHandler = messageHandler;
  }
};

// --- Test Fixture ---

class PeerTest : public Test {
protected:
  boost::asio::io_context io;
  std::shared_ptr<MockTorrentSession> session;
  std::shared_ptr<MockPeerConnection> conn;
  std::shared_ptr<MockPieceRepository> repo;
  std::shared_ptr<MockPiecePicker> picker;
  std::shared_ptr<Peer> peer;

  void SetUp() override {
    session = std::make_shared<MockTorrentSession>();
    conn = std::make_shared<MockPeerConnection>(io);
    repo = std::make_shared<MockPieceRepository>();
    picker = std::make_shared<MockPiecePicker>();

    // Inject all dependencies
    peer = std::make_shared<Peer>(conn, "127.0.0.1", repo, picker);
  }
};

// --- Tests ---

// --- doAction ---

TEST_F(PeerTest, RequestPiece_ShouldAskPickerForWork) {

  // Setup state
  peer->startAsOutbound({}, "peer_id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  // Expectation: Peer asks Repo for current bitfield to determine needs
  EXPECT_CALL(*repo, getBitfield()).WillRepeatedly(Return(std::vector<uint8_t>{}));

  // Expectation: Peer asks Picker for the next piece assignment
  EXPECT_CALL(*picker, pickPiece(_, _)).WillOnce(Return(4)); 

  // Expectation: Peer asks Repo for dimensions to calculate buffer size
  EXPECT_CALL(*repo, getPieceLength()).WillRepeatedly(Return(16384));
  EXPECT_CALL(*repo, getTotalLength()).WillRepeatedly(Return(1024 * 1024));

  // Expectation: Peer sends a Request message to the remote peer
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(4, 0, 16384))).Times(AtLeast(1));

  peer->doAction();
}

TEST_F(PeerTest, RequestPiece_ShouldStopIfPickerReturnsNone) {

  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  // Expectation: Repo returns empty bitfield
  EXPECT_CALL(*repo, getBitfield()).WillRepeatedly(Return(std::vector<uint8_t>{}));

  // Expectation: Picker returns std::nullopt (No work available)
  EXPECT_CALL(*picker, pickPiece(_, _)).WillOnce(Return(std::nullopt));

  // Expectation: No requests sent
  EXPECT_CALL(*conn, sendMessage(6, _)).Times(0);

  peer->doAction();
}

// --- handlePiece (Complete) ---

TEST_F(PeerTest, HandlePiece_Complete_ShouldVerifyAndSave) {
  
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  const int PIECE_IDX = 0;
  const int BLOCK_SIZE = 16384;

  // --- SETUP: Assign Piece 0 ---
  // Expectation: Initial check for work and assignment
  EXPECT_CALL(*repo, getBitfield()).WillRepeatedly(Return(std::vector<uint8_t>{}));
  EXPECT_CALL(*picker, pickPiece(_, _)).WillOnce(Return(PIECE_IDX));
  EXPECT_CALL(*repo, getPieceLength()).WillRepeatedly(Return(BLOCK_SIZE));
  EXPECT_CALL(*repo, getTotalLength()).WillRepeatedly(Return(1024*1024));
  
  // Expectation: Send initial request
  EXPECT_CALL(*conn, sendMessage(6, _));

  peer->doAction();

  // --- ACTION: Receive Piece Data ---
  PeerMessage msg;
  msg.id = 7;
  uint32_t idx_net = htonl(PIECE_IDX);
  uint32_t begin_net = htonl(0);
  msg.payload.resize(8 + BLOCK_SIZE);
  memcpy(msg.payload.data(), &idx_net, 4);
  memcpy(msg.payload.data() + 4, &begin_net, 4);
  // Data is zero-init

  // --- EXPECTATIONS ---
  // Expectation: Peer verifies the downloaded piece hash
  EXPECT_CALL(*repo, verifyHash(PIECE_IDX, _)).WillOnce(Return(true));
  
  // Expectation: Peer saves the verified piece to disk
  EXPECT_CALL(*repo, savePiece(PIECE_IDX, _));

  // Expectation: Peer notifies Picker that the piece was successfully downloaded
  EXPECT_CALL(*picker, onPiecePassed(PIECE_IDX));

  // Expectation: Peer immediately attempts to request the next piece (no more work here)
  EXPECT_CALL(*picker, pickPiece(_, _)).WillOnce(Return(std::nullopt)); 

  conn->storedMessageHandler(boost::system::error_code(), msg);
}

// --- Bad Hash ---

TEST_F(PeerTest, HandlePiece_BadHash_ShouldReportFailure) {
  
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  const int PIECE_IDX = 0;
  const int BLOCK_SIZE = 16384;


  // Expectation: Initial work assignment setup
  EXPECT_CALL(*repo, getBitfield()).WillRepeatedly(Return(std::vector<uint8_t>{}));
  EXPECT_CALL(*picker, pickPiece(_, _)).WillRepeatedly(Return(PIECE_IDX)); 
  EXPECT_CALL(*repo, getPieceLength()).WillRepeatedly(Return(BLOCK_SIZE));
  EXPECT_CALL(*repo, getTotalLength()).WillRepeatedly(Return(1024*1024));
  EXPECT_CALL(*conn, sendMessage(6, _)).Times(AtLeast(1));

  peer->doAction();

  PeerMessage msg;
  msg.id = 7;
  uint32_t idx_net = htonl(PIECE_IDX);
  uint32_t begin_net = htonl(0);
  msg.payload.resize(8 + BLOCK_SIZE);
  memcpy(msg.payload.data(), &idx_net, 4);
  memcpy(msg.payload.data() + 4, &begin_net, 4);

  // Expectation: Verify Hash -> FAIL
  EXPECT_CALL(*repo, verifyHash(PIECE_IDX, _)).WillOnce(Return(false));
  
  // Expectation: Save -> SHOULD NOT HAPPEN on failure
  EXPECT_CALL(*repo, savePiece(_, _)).Times(0);

  // Expectation: Notify Picker of failure to reschedule piece
  EXPECT_CALL(*picker, onPieceFailed(PIECE_IDX));

  conn->storedMessageHandler(boost::system::error_code(), msg);
}

TEST_F(PeerTest, ShouldDisconnectAfterThreeBadHashes) {
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;
  const int BLOCK_SIZE = 16384;

  // Setup mocks to always fail hash
  // Expectation: Standard setup to get into downloading state
  EXPECT_CALL(*repo, getBitfield()).WillRepeatedly(Return(std::vector<uint8_t>{}));
  EXPECT_CALL(*picker, pickPiece(_, _)).WillRepeatedly(Return(0)); 
  EXPECT_CALL(*repo, getPieceLength()).WillRepeatedly(Return(BLOCK_SIZE));
  EXPECT_CALL(*repo, getTotalLength()).WillRepeatedly(Return(1024*1024));
  EXPECT_CALL(*conn, sendMessage(6, _)).Times(AnyNumber());

  peer->doAction(); // Start 1st attempt

  PeerMessage msg;
  msg.id = 7; msg.payload.resize(8 + BLOCK_SIZE);
  memset(msg.payload.data(), 0, 8); // Index 0, Begin 0

  // Attempt 1
  // Expectation: Hash fails, notify picker
  EXPECT_CALL(*repo, verifyHash(0, _)).WillOnce(Return(false));
  EXPECT_CALL(*picker, onPieceFailed(0));
  conn->storedMessageHandler(boost::system::error_code(), msg);
  
  peer->doAction(); // Start 2nd attempt

  // Attempt 2
  // Expectation: Hash fails, notify picker
  EXPECT_CALL(*repo, verifyHash(0, _)).WillOnce(Return(false));
  EXPECT_CALL(*picker, onPieceFailed(0));
  conn->storedMessageHandler(boost::system::error_code(), msg);
  
  peer->doAction(); // Start 3rd attempt

  // Attempt 3
  // Expectation: Hash fails, notify picker
  EXPECT_CALL(*repo, verifyHash(0, _)).WillOnce(Return(false));
  EXPECT_CALL(*picker, onPieceFailed(0));
  
  // Expectation: Connection closed due to excessive protocol errors (bad data)
  EXPECT_CALL(*conn, close(Property(&boost::system::error_code::value, static_cast<int>(boost::system::errc::protocol_error))));

  conn->storedMessageHandler(boost::system::error_code(), msg);
}

TEST_F(PeerTest, HandleBitfield_ShouldNotifyPicker) {
  peer->startAsOutbound({}, "id", session);

  std::vector<uint8_t> payload = { 0b10100000 };
  PeerMessage msg;
  msg.id = 5;
  msg.payload = payload;

  // Expectation: Picker gets notified of peer's bitfield to update availability
  EXPECT_CALL(*picker, processBitfield(payload));

  conn->storedMessageHandler(boost::system::error_code(), msg);
}