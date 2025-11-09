#ifndef PEER_H
#define PEER_H

#include <boost/asio.hpp> // Main Asio header
#include <string>
#include <vector>
#include <optional>
#include <memory>


// Use a convenience namespace
namespace asio = boost::asio;
using asio::ip::tcp;

/**
 * @brief Represents a single message received from a peer.
 */
struct PeerMessage {
  uint8_t id;
  std::vector<unsigned char> payload;
};

/**
 * @brief Holds info about a block request we are waiting for.
 */
struct PendingRequest {
    uint32_t pieceIndex;
    uint32_t begin;
    uint32_t length;
};

/**
 * @brief Manages a single TCP connection to a BitTorrent peer.
 */
class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
public:

  /**
   * @brief Constructs a peer connection from ip and port
   * 
   * This is for OUTBOUND connections (From a peer list)
   * 
   * @param io_context The single, shared Asio io_context.
   * @param peer_ip The IP address of the peer to connect to.
   * @param peer_port The port of the peer to connect to.
   */
  PeerConnection(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port);

  /**
   * @brief Constructor a peer connection from an existing socket
   * 
   * This is for INBOUND connections
   * 
   * @param io_context The single, shared Asio io_context.
   * @param socket An already-connected socket from a tcp::acceptor.
   */
  PeerConnection(asio::io_context& io_context, tcp::socket socket);

  /**
   * @brief Starts the connection process for an OUTBOUND connection.
   *
   * This will connect, handshake, send bitfield, and start the message loop.
   */
  void startAsOutbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    long long pieceLength, 
    long long totalLength, 
    size_t numPieces, 
    std::vector<uint8_t>* myBitfield,
    std::string* pieceHashes
  );

  /**
   * @brief Starts the connection process for an INBOUND connection.
   *
   * This will receive a handshake, reply, send bitfield, and start the message loop.
   */
  void startAsInbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    long long pieceLength, 
    long long totalLength, 
    size_t numPieces, 
    std::vector<uint8_t>* myBitfield,
    std::string* pieceHashes
  );

  /**
   * @brief Attempts to synchronously connect to the peer.
   * @throws std::exception on connection failure.
   */
  void connect();

  /**
   * @brief Performs the 68-byte BitTorrent handshake.
   * @param infoHash The 20-byte info_hash.
   * @param peerId Our 20-byte peer_id.
   * @return The 20-byte peer_id from the other client, or an empty vector on failure.
   */
  std::vector<unsigned char> performHandshake();

  /**
   * @brief Constructs and sends a Bitfield message (ID=5) to the peer.
   * @param bitfield The raw bytes of our bitfield.
   * @throws std::runtime_error on send failure.
   */
  void sendBitfield(const std::vector<uint8_t>& bitfield);

  /**
   * @brief Sends an Interested message (ID 2).
   */
  void sendInterested();

  /**
   * @brief Sends a Request message (ID 6).
   * 
   * The length should be 2^14 (16384) byte according to
   * https://wiki.theory.org/BitTorrentSpecification#Message_flow
   * 
   * @param pieceIndex The zero-based index of the piece.
   * @param begin The zero-based byte offset within the piece.
   * @param length The requested length of the block (e.g., 16384).
   */
  void sendRequest(uint32_t pieceIndex, uint32_t begin, uint32_t length);

  /**
   * @brief Updates the peer's bitfield to indicate they have a piece.
   * @param pieceIndex The zero-based index of the piece.
   */
  void setHavePiece(uint32_t pieceIndex);

  /**
   * @brief Checks if the peer has a specific piece, based on their bitfield.
   * @param pieceIndex The zero-based index of the piece.
   * @return True if the peer has the piece, false otherwise.
   */
  bool hasPiece(uint32_t pieceIndex) const;

  // --- Peer State Variables ---
  // These variables store the state of this peer.
  
  // Proabably need more but the specifications say
  // this should be enough even for more effecient algorithms
  
  /** @brief Bitfield of the peer */
  std::vector<uint8_t> bitfield_;
  
  /** @brief Are we choking this peer? (i.e., not sending them data) */
  bool amChoking_ = true;
  
  /** @brief Is this peer choking us? (i.e., not willing to send us data) */
  bool peerChoking_ = true;
  
  /** @brief Are we interested in what this peer has? */
  bool amInterested_ = false;
  
  /** @brief Is this peer interested in what we have? */
  bool peerInterested_ = false;

private:

    // --- Inbound Handshake Logic ---
  void asyncReadInboundHandshake();
  void handleReadInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred);
  void asyncWriteInboundHandshake();
  void handleWriteInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred);
  void asyncWriteInboundBitfield();
  void handleWriteInboundBitfield(const boost::system::error_code& ec, size_t bytesTransferred);


  // --- Async Read Loop ---

  /**
   * @brief Begins the asyncronous read for the 4-byte message header
   * 
   * Calls handleReadHeader as the completion handler
   */
  void startAsyncRead();

  /**
   * @brief Begins an asynchronous read for the message body (ID + payload).
   * 
   * Calls handleReadBody as the completion handler
   * 
   * @param msgLength The length of the body to read.
   */
  void startAsyncReadBody(uint32_t msgLength);

  /**
   * @brief Handles the 4-byte message header and checks that read has succeeded
   * 
   * Starts the aynchronous reading of the body
   * 
   * Enforces the 16KB max payload
   * https://www.bittorrent.org/beps/bep_0003.html
   * 
   * @param ec Error code passed in by asio:async_read
   * 
   * Calls handleReadHeader as the completion handler
   */
  void handleReadHeader(const boost::system::error_code& ec);

  /**
   * @brief Handles the message from the peer
   * 
   * Completion handler for startAsyncReadBody
   * 
   * Handles the message via state changing and action functions
   * 
   * @param ec Error code passed in by asio:async_read
   * 
   * Calls handleReadHeader as the completion handler
   */
  void handleReadBody(uint32_t msgLength, const boost::system::error_code& ec);

  // --- Message Handlers (State Changers) ---

  /**
   * @brief Main message router
   * @param peer The peer that sent the message.
   * @param msg The message received from the peer.
   */
  void handleMessage(PeerMessage msg);

  /** 
   * @brief Handles a Choke message (ID 0) 
   */
  void handleChoke();
  /** 
   * @brief Handles an Unchoke message (ID 1)
   */
  void handleUnchoke();
  /** 
   * @brief Handles a Have message (ID 4) 
   */
  void handleHave(const PeerMessage& msg);
  /** 
   * @brief Handles a Bitfield message (ID 5) 
   */
  void handleBitfield( const PeerMessage& msg);
  /** 
   * @brief Handles a Piece message (ID 7) 
   */
  void handlePiece(const PeerMessage& msg);

  // --- State Actions (Handles Actions Dependent on State) ---

  /**
   * @brief Sends requests for pieces not haven to a peer
   * 
   * Only requests if the buffer is not full
   * AND
   * Peer has piece client wants
   * 
   * Otherwise fills up the buffer to the max
   */
  void requestPiece();

  void doAction();

  /** 
   * @brief Checks if we are interested in the peer and sends an
   * Interested message (ID 2) if we aren't already.
   * 
   * We are interested if the peer has a piece we don't
   */
  void checkAndSendInterested();

  // --- Helper Functions ---
  
  bool clientHasPiece(size_t pieceIndex) const;
  bool verifyPieceHash(size_t pieceIndex);

  /**
   * @brief Sends a generic message to the peer.
   * Client doesn't use this, each message will have own function
   * Prepends the 4-byte length and 1-byte ID.
   */
  void sendMessage(uint8_t id, const std::vector<unsigned char>& payload);

  // --- Download State ---
  std::vector<PendingRequest> inFlightRequests_;
  static const int MAX_PIPELINE_SIZE = 5;
  
  size_t nextPieceIndex_ = 0;
  uint32_t nextBlockOffset_ = 0;
  std::vector<uint8_t> currentPieceBuffer_;
  // Whether or not the bitfield has been updated since last action
  bool bitfieldUpdated_;

  // --- Torrent Info (passed from Client) ---
  long long pieceLength_ = 0;
  long long totalLength_ = 0;
  size_t numPieces_ = 0;
  std::vector<uint8_t>* myBitfield_; // Pointer to client's bitfield
  std::string* pieceHashes_;    // Pointer to client's hashes
  std::vector<unsigned char> infoHash_; 
  std::string peerId_;

  // --- Asio Variables ---
  std::string ip_;
  std::string port_str_;
  tcp::socket socket_; // The socket for this connection

  // Buffers for async reading
  std::vector<uint8_t> readHeaderBuffer_; // 4-byte length prefix
  std::vector<uint8_t> readBodyBuffer_;   // For message ID + payload
  std::vector<uint8_t> handshakeBuffer_;  // 68 bytes
};

#endif // PEER_H
