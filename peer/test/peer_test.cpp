#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "peer.h"
#include "ITorrentSession.h"
#include "peerConnection.h"
#include <cstring>
#include <arpa/inet.h> // For ntohl
#include <openssl/sha.h>

using namespace testing;

// --- Custom Matcher to verify Binary Payload ---
// This checks if a std::vector<uint8_t> contains the specific
// [index, begin, length] encoded in Big Endian.
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
    MOCK_METHOD(void, onPieceCompleted, (size_t, std::vector<uint8_t>), (override));
    MOCK_METHOD(void, onBitfieldReceived, (std::shared_ptr<Peer>, std::vector<uint8_t>), (override));
    MOCK_METHOD(void, onHaveReceived, (std::shared_ptr<Peer>, size_t), (override));
    MOCK_METHOD(std::optional<size_t>, assignWorkForPeer, (std::shared_ptr<Peer>), (override));
    MOCK_METHOD(void, unassignPiece, (size_t), (override));
    
    // Getters
    MOCK_METHOD(long long, getPieceLength, (), (const, override));
    MOCK_METHOD(long long, getTotalLength, (), (const, override));
    MOCK_METHOD(const std::vector<uint8_t>&, getBitfield, (), (const, override));
    MOCK_METHOD(bool, clientHasPiece, (size_t), (const, override));
    MOCK_METHOD(const char*, getPieceHash, (size_t), (const, override));
    MOCK_METHOD(void, updateMyBitfield, (size_t), (override));
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
  std::shared_ptr<Peer> peer;

  void SetUp() override {
    //
    session = std::make_shared<MockTorrentSession>();
    conn = std::make_shared<MockPeerConnection>(io);
    peer = std::make_shared<Peer>(conn, "127.0.0.1");
  }
};

// --- Tests ---

// --- doAction ---

TEST_F(PeerTest, RequestPiece_ShouldAskSessionForWorkAndSendRequest) {

  // Client interested
  // Peer not choking
  peer->startAsOutbound({}, "peer_id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  // When client is interested and peer is not choking
  // assignWorkForPeer should be called
  EXPECT_CALL(*session, assignWorkForPeer(_))
      .WillOnce(Return(4)); 

  // The Peer asks for dimensions to resize its buffer.
  EXPECT_CALL(*session, getPieceLength())
      .WillRepeatedly(Return(16384)); // Standard 16KB blocks
  EXPECT_CALL(*session, getTotalLength())
      .WillRepeatedly(Return(1024 * 1024)); // 1MB total size

  // The Peer should send a "Request" message (ID = 6).
  // Expect: Index 4, Offset 0, Length 16384.
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(4, 0, 16384)))
      .Times(AtLeast(1));

  // This should call requestPiece
  peer->doAction();
}

TEST_F(PeerTest, RequestPiece_ShouldStopIfSessionHasNoWork) {

  // Client interested
  // Peer not choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  // Session returns std::nullopt (No work available)
  EXPECT_CALL(*session, assignWorkForPeer(_))
    .WillOnce(Return(std::nullopt));

  // Expectation: Connection should NOT receive any request messages
  EXPECT_CALL(*conn, sendMessage(6, _)).Times(0);

  peer->doAction();
}

TEST_F(PeerTest, ShouldNotRequestPieceWhenNotInterested) {

  // Client NOT interested
  // Peer not choking
  peer->startAsOutbound({}, "id", session);
  peer->peerChoking_ = false;
  peer->amInterested_ = false;

  // Expectation: The peer should NOT ask the session for work
  EXPECT_CALL(*session, assignWorkForPeer(_)).Times(0);
  
  // Expectation: The peer should NOT send any messages
  EXPECT_CALL(*conn, sendMessage(_, _)).Times(0);

  peer->doAction();
}

TEST_F(PeerTest, ShouldNotRequestPieceWhenChoked) {

  // Client interested
  // Peer IS choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = true;

  // Expectation: The peer should NOT ask the session for work
  EXPECT_CALL(*session, assignWorkForPeer(_)).Times(0);
  
  // Expectation: The peer should NOT send any messages
  EXPECT_CALL(*conn, sendMessage(_, _)).Times(0);

  peer->doAction();
}

// --- handleChoke ---

TEST_F(PeerTest, HandleChoke_ShouldResetRequestPipeline) {
  
  // Client IS interested
  // Peer NOT choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  const int PIECE_IDX = 10;
  const int PIECE_LEN = 81920;

  // Expectation: The peer should ask the session for work
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(PIECE_IDX));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(PIECE_LEN));
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(1024 * 1024));

  // Expectation: The peer should fill the request pipeline
  {
    InSequence s;
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 0, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 16384, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 32768, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 49152, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 65536, 16384)));
  }

  peer->doAction();

  // Action: Receive CHOKE
  PeerMessage msg;
  msg.id = 0; // Choke
  
  // Expectation: The peer should NOT unassign the piece
  EXPECT_CALL(*session, unassignPiece(_)).Times(0);
  
  // Expectation: The peer state should be set to choking
  conn->storedMessageHandler(boost::system::error_code(), msg);
  EXPECT_TRUE(peer->peerChoking_);

  // Action: Receive UNCHOKE
  msg.id = 1; // Unchoke

  // Expectation: The peer should ask the session for work
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(PIECE_IDX));

  // Expectation: The peer should fill the request pipeline from the last confirmed block
  // (Which is the 0th block)
  {
    InSequence s;
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 0, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 16384, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 32768, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 49152, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 65536, 16384)));
  }

  conn->storedMessageHandler(boost::system::error_code(), msg);
  EXPECT_FALSE(peer->peerChoking_);
}

TEST_F(PeerTest, HandleChoke_ShouldRequestFromLastConfirmedPiece) {


  // Client IS interested
  // Peer NOT choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  const int PIECE_IDX = 8;
  const int PIECE_LEN = 98304; // 6 blocks (16384 * 6)

  // Expectation: The peer should ask the session for work
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(PIECE_IDX));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(PIECE_LEN));
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(1024 * 1024));

  // Expectation: The peer should fill the request pipeline
  {
    InSequence s;
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 0, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 16384, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 32768, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 49152, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 65536, 16384)));
  }
  
  peer->doAction(); // Triggers first 5 requests

  // Peer receives data for the first request
  PeerMessage pieceMsg;
  pieceMsg.id = 7; // Piece
  
  // Construct payload: Index(4) + Begin(4) + Data(16384)
  uint32_t net_idx = htonl(PIECE_IDX);
  uint32_t net_begin = htonl(0); // Offset 0
  pieceMsg.payload.resize(8 + 16384);
  memcpy(pieceMsg.payload.data(), &net_idx, 4);
  memcpy(pieceMsg.payload.data() + 4, &net_begin, 4);

  // Expectation: The peer should request Block 5 (Offset 81920)
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 81920, 16384)));

  conn->storedMessageHandler(boost::system::error_code(), pieceMsg);

  // Expectation: The peer gets choked and updates state
  PeerMessage chokeMsg;
  chokeMsg.id = 0; 
  conn->storedMessageHandler(boost::system::error_code(), chokeMsg);
  EXPECT_TRUE(peer->peerChoking_);

  // --- Step 4: Get Unchoked (Resume) ---
  PeerMessage unchokeMsg;
  unchokeMsg.id = 1;

  // Expectation: The peer should requests blocks AFTER first block
  // It should NOT request Offset 0.
  // It SHOULD request Offset 16384, 32768, etc. (Blocks 1-5).
  {
    InSequence s;
    // Note: 16384 is the first one, NOT 0.
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 16384, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 32768, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 49152, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 65536, 16384)));
    EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 81920, 16384)));
  }

  conn->storedMessageHandler(boost::system::error_code(), unchokeMsg);
}

// --- handleUnchoke ---

TEST_F(PeerTest, HandleUnchoke_ShouldUpdateStateAndTriggerRequests) {

  // Client interested
  // Peer IS choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = true;

  // Expectation: The peer should ask the session for work
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(0));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(81920));
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(1024 * 1024));

  // Expectation: The peer should fill up the request pipeline to the max (5)
  //              with correct offsets
  InSequence s;
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(0, 0, 16384)));
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(0, 16384, 16384)));
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(0, 32768, 16384)));
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(0, 49152, 16384)));
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(0, 65536, 16384)));

  // Send unchoke
  PeerMessage msg;
  msg.id = 1;
  conn->storedMessageHandler(boost::system::error_code(), msg);

  // Expectation: The Peer choking should be updated
  EXPECT_FALSE(peer->peerChoking_);
}

TEST_F(PeerTest, RequestPiece_ShouldThrowIfAssignedPieceIsOutOfBounds) {
 
  // Client interested
  // Peer NOT choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  // Expectation: The peer should ask the session for work
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(10));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(100)); 
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(500)); 

  // Expectation: Peer throws an error since 10 * 100 (offset) is greater than file
  EXPECT_THROW(peer->doAction(), std::runtime_error);
}

// --- handleBitfield ---

TEST_F(PeerTest, HandleBitfield_ShouldUpdateStateAndNotifySession) {


  peer->startAsOutbound({}, "id", session);

  // Bitmask: 10100000 
  // Pieces 0 and 2 are present
  std::vector<uint8_t> payload = { 0b10100000 };
  
  PeerMessage msg;
  msg.id = 5; // Bitfield
  msg.payload = payload;

  // Expectation: The peer should call onBitfieldRecieved
  // with proper payload
  EXPECT_CALL(*session, onBitfieldReceived(_, payload));

  conn->storedMessageHandler(boost::system::error_code(), msg);

  // Expectation: The peer's hasPiece method
  // should return proper values for indices
  EXPECT_TRUE(peer->hasPiece(0));
  EXPECT_FALSE(peer->hasPiece(1));
  EXPECT_TRUE(peer->hasPiece(2));
  EXPECT_FALSE(peer->hasPiece(3));
}

// --- handlePiece

TEST_F(PeerTest, HandlePiece_Incomplete_ShouldRefillPipeline) {

  // Client interested
  // Peer NOT choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  const int PIECE_IDX = 0;
  const int BLOCK_SIZE = 16384;
  const int PIECE_LEN = BLOCK_SIZE * 6;

  // Expectation: The peer should fill request pipeline
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(PIECE_IDX));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(PIECE_LEN));
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(1024*1024));
  EXPECT_CALL(*conn, sendMessage(6, _)).Times(5);

  peer->doAction();

  // Prepare PIECE message for Block 0 (Offset 0)
  PeerMessage msg;
  msg.id = 7;
  uint32_t idx_net = htonl(PIECE_IDX);
  uint32_t begin_net = htonl(0);
  msg.payload.resize(8 + BLOCK_SIZE); // Header + Data
  memcpy(msg.payload.data(), &idx_net, 4);
  memcpy(msg.payload.data() + 4, &begin_net, 4);
  // (Data left as zeros)

  // Expectation: The peer should NOT call onPieceCompleted
  EXPECT_CALL(*session, onPieceCompleted(_, _)).Times(0);

  // Expectation: The peer should request the next block
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_IDX, 81920, BLOCK_SIZE)));

  conn->storedMessageHandler(boost::system::error_code(), msg);
}

TEST_F(PeerTest, HandlePiece_Complete_ShouldVerifyAndRequestNext) {
  
  // Client interested
  // Peer NOT choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  const int PIECE_0 = 0;
  const int PIECE_1 = 1;
  const int BLOCK_SIZE = 16384;

  // Expectation: The peer should fill request pipeline
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(PIECE_0));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(BLOCK_SIZE));
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(1024*1024));
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_0, 0, BLOCK_SIZE)));

  peer->doAction();

  // Prepare Data and Hash
  std::vector<unsigned char> data(BLOCK_SIZE, 'A');
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(data.data(), data.size(), hash);

  // Prepare PIECE Message
  PeerMessage msg;
  msg.id = 7;
  uint32_t idx_net = htonl(PIECE_0);
  uint32_t begin_net = htonl(0);
  msg.payload.resize(8 + BLOCK_SIZE);
  memcpy(msg.payload.data(), &idx_net, 4);
  memcpy(msg.payload.data() + 4, &begin_net, 4);
  memcpy(msg.payload.data() + 8, data.data(), BLOCK_SIZE);

  // Expectation: The peer should ask for proper piece hash
  EXPECT_CALL(*session, getPieceHash(PIECE_0))
      .WillOnce(Return(reinterpret_cast<const char*>(hash)));
  
  // Expectation: The peer should update bitfield in session
  EXPECT_CALL(*session, updateMyBitfield(PIECE_0));

  // Expectation: The peer should call onPieceCompleted
  EXPECT_CALL(*session, onPieceCompleted(PIECE_0, _));

  // 4. Expectations for NEXT Piece
  // Peer must automatically ask for more work (Piece 1) and request it
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(PIECE_1));
  EXPECT_CALL(*conn, sendMessage(6, HasRequestPayload(PIECE_1, 0, BLOCK_SIZE)));

  conn->storedMessageHandler(boost::system::error_code(), msg);
}

TEST_F(PeerTest, HandlePiece_ShouldIgnoreBlocksNotInRequestQueue) {
  
  // Client interested
  // Peer NOT choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  // Expectation: The peer should fill request pipeline
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(0));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(16384));
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(1024*1024));
  EXPECT_CALL(*conn, sendMessage(6, _)).Times(AtLeast(1));

  peer->doAction(); // Request sent for Piece 0, Offset 0

  // Prepare unsolicited PIECE message (Piece 99)
  PeerMessage msg;
  msg.id = 7;
  uint32_t idx_net = htonl(99); // Wrong index
  uint32_t begin_net = htonl(0);
  msg.payload.resize(8 + 16384);
  memcpy(msg.payload.data(), &idx_net, 4);
  memcpy(msg.payload.data() + 4, &begin_net, 4);

  // Expectation: The peer should not complete the piece
  EXPECT_CALL(*session, onPieceCompleted(_, _)).Times(0);

  // Expectation: The peer should not request the next block
  EXPECT_CALL(*conn, sendMessage(6, _)).Times(0);

  conn->storedMessageHandler(boost::system::error_code(), msg);
}

TEST_F(PeerTest, HandlePiece_BadHash_ShouldUnassignAndReset) {
  
  // Client interested
  // Peer NOT choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  const int PIECE_IDX = 0;
  const int BLOCK_SIZE = 16384;

  // Expectation: The peer should fill request pipeline
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillRepeatedly(Return(PIECE_IDX));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(BLOCK_SIZE));
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(1024*1024));
  EXPECT_CALL(*conn, sendMessage(6, _)).Times(AtLeast(1));

  peer->doAction();

  std::vector<unsigned char> data(BLOCK_SIZE, 'A');
  
  // Mock BAD Hash from Session (Expects hash of 'B's)
  unsigned char badHash[SHA_DIGEST_LENGTH];
  std::vector<unsigned char> badData(BLOCK_SIZE, 'B');
  SHA1(badData.data(), badData.size(), badHash);

  // Expectation: The peer should call getPieceHash to compare completed piece
  EXPECT_CALL(*session, getPieceHash(PIECE_IDX))
      .WillOnce(Return(reinterpret_cast<const char*>(badHash)));

  // Prepare PIECE Message
  PeerMessage msg;
  msg.id = 7;
  uint32_t idx_net = htonl(PIECE_IDX);
  uint32_t begin_net = htonl(0);
  msg.payload.resize(8 + BLOCK_SIZE);
  memcpy(msg.payload.data(), &idx_net, 4);
  memcpy(msg.payload.data() + 4, &begin_net, 4);
  memcpy(msg.payload.data() + 8, data.data(), BLOCK_SIZE);

  // Expectation: The peer should NOT call onPieceCompleted
  EXPECT_CALL(*session, onPieceCompleted(_, _)).Times(0);

  // Expectation: Should call unassignPiece so it can be tried again
  EXPECT_CALL(*session, unassignPiece(PIECE_IDX));

  conn->storedMessageHandler(boost::system::error_code(), msg);
}

TEST_F(PeerTest, ShouldDisconnectAfterThreeBadHashes) {
  
  // Client interested
  // Peer NOT choking
  peer->startAsOutbound({}, "id", session);
  peer->amInterested_ = true;
  peer->peerChoking_ = false;

  const int BLOCK_SIZE = 16384;
  
  // Create a test payload
  PeerMessage msg;
  msg.id = 7; // Piece
  uint32_t idx_net = htonl(0);
  uint32_t begin_net = htonl(0);
  msg.payload.resize(8 + BLOCK_SIZE);
  memcpy(msg.payload.data(), &idx_net, 4);
  memcpy(msg.payload.data() + 4, &begin_net, 4);
  // Data is all zeros

  // Calculate hash
  unsigned char realHash[SHA_DIGEST_LENGTH];
  std::vector<unsigned char> data(BLOCK_SIZE, 0);
  SHA1(data.data(), data.size(), realHash);

  // Bad hash is just 'X'
  char badHash[20] = { 'X' };

  // Expectation: The peer should fill request pipeline
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(0));
  EXPECT_CALL(*session, getPieceLength()).WillRepeatedly(Return(BLOCK_SIZE));
  EXPECT_CALL(*session, getTotalLength()).WillRepeatedly(Return(1024*1024));
  EXPECT_CALL(*conn, sendMessage(6, _));

  peer->doAction(); // Start downloading

  // Expectation: The peer should reject the piece and unAssign it
  EXPECT_CALL(*session, getPieceHash(0)).WillOnce(Return(badHash));
  EXPECT_CALL(*session, unassignPiece(0));
  EXPECT_CALL(*conn, close(_)).Times(0); // Should NOT close yet

  // Expectation: The peer should ask for next piece
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(0));
  EXPECT_CALL(*conn, sendMessage(6, _));

  conn->storedMessageHandler(boost::system::error_code(), msg); // Receive Piece

  // Expectation: The peer should ask for work again
  EXPECT_CALL(*session, assignWorkForPeer(_)).WillOnce(Return(0));
  EXPECT_CALL(*conn, sendMessage(6, _));
  peer->doAction(); 

  // Expectation: The peer should reject the piece and unAssign it
  EXPECT_CALL(*session, getPieceHash(0)).WillOnce(Return(badHash));
  EXPECT_CALL(*session, unassignPiece(0));
  EXPECT_CALL(*conn, close(_)).Times(0);

  conn->storedMessageHandler(boost::system::error_code(), msg); // Receive Piece

  // Expectation: The peer should reject the piece and unAssign it
  //              AND disconnect early
  EXPECT_CALL(*session, getPieceHash(0)).WillOnce(Return(badHash));
  EXPECT_CALL(*session, unassignPiece(0)).Times(0); 
  EXPECT_CALL(*conn, close(Property(&boost::system::error_code::value, static_cast<int>(boost::system::errc::protocol_error))));

  conn->storedMessageHandler(boost::system::error_code(), msg); // Receive Piece
}