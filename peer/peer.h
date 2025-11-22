#ifndef PEER_H
#define PEER_H

#include "peerConnection.h"
#include <string>
#include <vector>
#include <optional>
#include <memory>


// Use a convenience namespace
namespace asio = boost::asio;
using asio::ip::tcp;

// Forward declaration for TorrentSession defined in TorrentSession.h
class TorrentSession;

/**
 * @brief Holds info about a block request we are waiting for.
 */
struct PendingRequest {
    uint32_t pieceIndex;
    uint32_t begin;
    uint32_t length;
};

/**
 * @brief Manages state and logic for a peer
 */
class Peer : public std::enable_shared_from_this<Peer> {
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
  Peer(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port);

  /**
   * @brief Constructor a peer connection from an existing socket
   * 
   * This is for INBOUND connections
   * 
   * @param io_context The single, shared Asio io_context.
   * @param socket An already-connected socket from a tcp::acceptor.
   */
  Peer(asio::io_context& io_context, tcp::socket socket);

  /**
   * @brief Starts the connection process for an OUTBOUND connection.
   */
  void startAsOutbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    std::weak_ptr<TorrentSession> session
  );

  /**
   * @brief Starts the connection process for an INBOUND connection.
   */
  void startAsInbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    std::weak_ptr<TorrentSession> session
  );

  // --- Message Senders ---

  /**
   * @brief Sends an Interested message (ID 2) 
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
   * @brief Sends a Bitfield message (ID 5).
   */
  void sendBitfield();

  void setAmInterested(bool interested);

  // /**
  //  * @brief Updates the peer's bitfield to indicate they have a piece.
  //  * @param pieceIndex The zero-based index of the piece.
  //  */
  // void setHavePiece(uint32_t pieceIndex);

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
  
  bool amChoking_ = true;
  bool peerChoking_ = true;
  bool amInterested_ = false;
  bool peerInterested_ = false;

private:

  // --- Callback handlers called by PeerConnection ---

  /**
   * @brief Called by PeerConnection when handshake is complete.
   */
  void onHandshakeComplete(const boost::system::error_code& ec, std::vector<unsigned char> peerId);

  /**
   * @brief Called by PeerConnection when a message arrives or connection drops.
   */
  void onMessageReceived(const boost::system::error_code& ec, std::optional<PeerMessage> msg);

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

  // Make better description.
  // This does the entire loop based on state 
  void doAction();

  // /** 
  //  * @brief Checks if we are interested in the peer and sends an
  //  * Interested message (ID 2) if we aren't already.
  //  * 
  //  * We are interested if the peer has a piece we don't
  //  */
  // void checkAndSendInterested();

  // --- Helper Functions ---
  
  /**
   * @brief Checks whether the client has the piece of pieceIndex in their bitfield
   * 
   * @param pieceIndex index of piece to check
   */
  bool clientHasPiece(size_t pieceIndex) const;

  /**
   * @brief Verifies the SHA-1 hash of the piece in currentPieceBuffer_.
   * 
   * @param pieceIndex index of piece to check the hash of
   */
  bool verifyPieceHash(size_t pieceIndex, std::shared_ptr<TorrentSession> session);

  /**
   * @brief Sends a generic message to the peer.
   * Client doesn't use this, each message will have own function
   * Prepends the 4-byte length and 1-byte ID.
   */
  void sendMessage(uint8_t id, const std::vector<unsigned char>& payload);

  /**
   * @brief Sets bit in peer bitfield
   * 
   * @param pieceIndex index of piece to set in bitfield
   */
  void setHavePiece(uint32_t pieceIndex);

  // --- Download State ---
  std::vector<PendingRequest> inFlightRequests_;
  static const int MAX_PIPELINE_SIZE = 5;
  
  size_t nextPieceIndex_ = 0;
  uint32_t nextBlockOffset_ = 0;
  std::vector<uint8_t> currentPieceBuffer_;

  // --- Connection ---
  std::string ip_; // For console logging
  std::shared_ptr<PeerConnection> conn_; // Socket connection layer

  // --- Session ---
  std::weak_ptr<TorrentSession> session_;

  /** @brief Bitfield of the peer */
  std::vector<uint8_t> bitfield_;

};

#endif // PEER_H
