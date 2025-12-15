#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "torrentSession.h"
#include "ITracker.h"
#include "ITorrentStorage.h"
#include "peer.h"
#include "peerConnection.h"
#include "torrent.h"

using namespace testing;

// --- Mocks ---

class MockTrackerClient : public ITrackerClient {
public:
  MOCK_METHOD(std::string, sendRequest, (const std::string&), (override));
};

class MockTorrentStorage : public ITorrentStorage {
public:
  MOCK_METHOD(void, initialize, (const TorrentData&, long long), (override));
  MOCK_METHOD(void, writePiece, (size_t, const std::vector<uint8_t>&), (override));
};

// We need a Stub Connection so we can instantiate a real Peer
class StubPeerConnection : public PeerConnection {
public:
    StubPeerConnection(boost::asio::io_context& io) : PeerConnection(io, "127.0.0.1", 0) {}
    
    // We capture the callback so we can simulate incoming messages
    MessageCallback storedMessageHandler;

    void startAsOutbound(
        const std::vector<unsigned char>&, 
        const std::string&, 
        HandshakeCallback, 
        MessageCallback messageHandler
    ) override {
        storedMessageHandler = messageHandler;
    }

    void sendMessage(uint8_t, const std::vector<unsigned char>&) override {} 
    void close(const boost::system::error_code&) override {}
};

class TorrentSessionTest : public Test {
protected:
  boost::asio::io_context io;
  std::string peerId = "-GT0001-123456789012";
  unsigned short port = 6882;
  std::shared_ptr<MockTrackerClient> mockTracker;
  std::shared_ptr<MockTorrentStorage> mockStorage;
  std::shared_ptr<TorrentSession> session;

  template <typename T>
  std::unique_ptr<BencodeValue> makeVal(T v) {
    auto val = std::make_unique<BencodeValue>();
    val->value = std::move(v);
    return val;
  }

  // Helper to create a minimal valid TorrentData object
  TorrentData createDummyTorrent() {
    TorrentData t;
    // Info Hash (20 bytes)
    t.infoHash.resize(20, 'a');
    
    // Construct minimal Bencoded Info Dictionary
    // Single file, 100 bytes total, 10 bytes per piece -> 10 pieces
    BencodeDict info;
    info.emplace("length", makeVal(100LL));
    info.emplace("name", makeVal(std::string("test_output.bin")));
    info.emplace("piece length", makeVal(10LL));
    
    std::string piecesStr(200, 'h');
    info.emplace("pieces", makeVal(piecesStr));
    
    // Wrap the dictionary itself and add to mainData using emplace
    t.mainData.emplace("info", makeVal(std::move(info)));
    t.mainData.emplace("announce", makeVal(std::string("http://tracker.com")));
    
    return t;
  }

  void SetUp() override {
    mockTracker = std::make_shared<MockTrackerClient>();
    mockStorage = std::make_shared<MockTorrentStorage>();
    
    // Create session
    TorrentData torrent = createDummyTorrent();
    session = std::make_shared<TorrentSession>(io, std::move(torrent), peerId, port, mockTracker, mockStorage);

    EXPECT_CALL(*mockTracker, sendRequest(_)).WillOnce(Return("de"));
    EXPECT_CALL(*mockStorage, initialize(_, _)); 
    
    // Manually trigger initialization (usually done by run() in client.cpp)
    try {
      session->start(); 
    } catch (...) {
      // Sould get error invalid dictionary
      // Ignore
    }
  }
  
  void TearDown() override {
    // Clean up the dummy file created
    std::remove("test_output.bin");
  }

  std::pair<std::shared_ptr<Peer>, std::shared_ptr<StubPeerConnection>> createPeerAndConn() {
    auto conn = std::make_shared<StubPeerConnection>(io);
    auto peer = std::make_shared<Peer>(conn, "127.0.0.1");
    
    // IMPORTANT: We must start the peer so it registers its callback with the connection
    // and gets a reference to the session.
    peer->startAsOutbound({}, "peer_id", session);
    
    return {peer, conn};
  }

  // Helper to simulate a peer having a piece
  void simulatePeerHave(std::shared_ptr<StubPeerConnection> conn, size_t pieceIndex) {
    PeerMessage msg;
    msg.id = 4; // Have Message
    
    uint32_t netIndex = htonl(pieceIndex);
    msg.payload.resize(4);
    std::memcpy(msg.payload.data(), &netIndex, 4);

    // Inject message
    if (conn->storedMessageHandler) {
        conn->storedMessageHandler(boost::system::error_code(), msg);
    }
  }
  
};

// --- assignWorkForPeer ---

// Same Rarity

TEST_F(TorrentSessionTest, AssignWork_ShouldAssignSequentiallySameRarity) {
  // Setup Peers and Connections
  auto [peer1, conn1] = createPeerAndConn();
  
  // Simulate peer1 having pieces 0, 1, 2 via messages
  // This updates Peer state AND notifies Session
  simulatePeerHave(conn1, 0);
  simulatePeerHave(conn1, 1);
  simulatePeerHave(conn1, 2);

  // First Request
  // Expectation: The session should assign the peer the first piece
  // SINCE it's the next rarest piece
  std::optional<size_t> work1 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work1.has_value());
  EXPECT_EQ(*work1, 0);

  // Second Request
  // Expectation: The session should assign the peer the second piece
  // SINCE it's the next rarest piece
  std::optional<size_t> work2 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work2.has_value());
  EXPECT_EQ(*work2, 1);

  // Third Request
  // Expectation: The session should assign the peer the third piece
  // SINCE it's the only available piece (and next rarest piece)
  std::optional<size_t> work3 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work3.has_value());
  EXPECT_EQ(*work3, 2);
}

TEST_F(TorrentSessionTest, AssignWork_TwoPeers_ShouldLockAndAssignSequentially) {
  // Setup Peers and Connections
  auto [peer1, conn1] = createPeerAndConn();
  auto [peer2, conn2] = createPeerAndConn();

  // Simulate peer1 having pieces 0, 1, 2 via messages
  // This updates Peer state AND notifies Session
  simulatePeerHave(conn1, 0);
  simulatePeerHave(conn1, 1);
  simulatePeerHave(conn1, 2);

  // Simulate peer2 having pieces 0, 1, 2 via messages
  simulatePeerHave(conn2, 0);
  simulatePeerHave(conn2, 1);
  simulatePeerHave(conn2, 2);

  // First Request (Peer 1)
  // Expectation: The session should assign the peer the first piece
  // SINCE it's the first available piece
  std::optional<size_t> work1 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work1.has_value());
  EXPECT_EQ(*work1, 0);

  // Second Request (Peer 2)
  // Expectation: The session should assign the peer the second piece
  // SINCE piece 0 is already locked by Peer 1
  std::optional<size_t> work2 = session->assignWorkForPeer(peer2);
  ASSERT_TRUE(work2.has_value());
  EXPECT_EQ(*work2, 1);

  // Third Request (Peer 1)
  // Expectation: The session should assign the peer the third piece
  // SINCE pieces 0 and 1 are both locked
  std::optional<size_t> work3 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work3.has_value());
  EXPECT_EQ(*work3, 2);
}

TEST_F(TorrentSessionTest, AssignWork_ThreePeers_ShouldExhaustAvailableWork) {
  // Setup Peers and Connections
  auto [peer1, conn1] = createPeerAndConn();
  auto [peer2, conn2] = createPeerAndConn();
  auto [peer3, conn3] = createPeerAndConn();

  // All peers have pieces 0, 1, 2
  for (int i = 0; i < 3; ++i) {
    simulatePeerHave(conn1, i);
    simulatePeerHave(conn2, i);
    simulatePeerHave(conn3, i);
  }

  // First Request (Peer 1)
  // Expectation: The session should assign the peer the first piece
  // SINCE it's the first available piece
  std::optional<size_t> work1 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work1.has_value());
  EXPECT_EQ(*work1, 0);

  // Second Request (Peer 2)
  // Expectation: The session should assign the peer the second piece
  // SINCE piece 0 is already locked by Peer 1
  std::optional<size_t> work2 = session->assignWorkForPeer(peer2);
  ASSERT_TRUE(work2.has_value());
  EXPECT_EQ(*work2, 1);

  // Third Request (Peer 1)
  // Expectation: The session should assign the peer the third piece
  // SINCE pieces 0 and 1 are both locked
  std::optional<size_t> work3 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work3.has_value());
  EXPECT_EQ(*work3, 2);

  // Fourth Request (Peer 3)
  // Expectation: The session should assign the peer NO piece 
  // SINCE Pieces 0, 1, 2 are all locked
  // AND Peer 3 has no other pieces.
  std::optional<size_t> work4 = session->assignWorkForPeer(peer3);
  EXPECT_FALSE(work4.has_value());
}

// --- Different Rarities ---

TEST_F(TorrentSessionTest, AssignWork_ShouldPrioritizeRarestPiece) {
  // Setup Peers and Connections
  auto [peer1, conn1] = createPeerAndConn();
  auto [peer2, conn2] = createPeerAndConn();

  // Peer 1 has 0, 1, 2
  simulatePeerHave(conn1, 0);
  simulatePeerHave(conn1, 1);
  simulatePeerHave(conn1, 2);

  // Peer 2 has 0, 1
  simulatePeerHave(conn2, 0);
  simulatePeerHave(conn2, 1);

  // Rarity:
  // Piece 0: 2 peers (Common)
  // Piece 1: 2 peers (Common)
  // Piece 2: 1 peer (Rare - only Peer 1 has it)

  // First Request (Peer 1)
  // Expectation: The session should assign the peer piece 3
  // SINCE it's the rarest piece
  std::optional<size_t> work1 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work1.has_value());
  EXPECT_EQ(*work1, 2);

  // Second Request (Peer 2)
  // Expectation: The session should assign the peer piece 0
  // SINCE it is the next rarest sequentially
  std::optional<size_t> work2 = session->assignWorkForPeer(peer2);
  ASSERT_TRUE(work2.has_value());
  EXPECT_EQ(*work2, 0);

  // Third Request (Peer 1)
  // Expectation: The session should assign the peer piece 1
  // SINCE pieces 0 and 2 are both locked
  std::optional<size_t> work3 = session->assignWorkForPeer(peer1);
  ASSERT_TRUE(work3.has_value());
  EXPECT_EQ(*work3, 1);
}