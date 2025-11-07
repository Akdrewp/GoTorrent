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
 * @brief Manages a single TCP connection to a BitTorrent peer.
 */
class PeerConnection {
public:
  /**
   * @brief Constructs a peer connection.
   * @param io_context The single, shared Asio io_context.
   * @param peer_ip The IP address of the peer to connect to.
   * @param peer_port The port of the peer to connect to.
   */
  PeerConnection(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port);

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
  std::vector<unsigned char> performHandshake(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId
  );

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
   * And the max is 2^15 (32768) byte
   * 
   * @param pieceIndex The zero-based index of the piece.
   * @param begin The zero-based byte offset within the piece.
   * @param length The requested length of the block (e.g., 16384).
   */
  void sendRequest(uint32_t pieceIndex, uint32_t begin, uint32_t length);


  /**
   * @brief Reads a single peer message from the socket.
   *
   * This is a blocking call. It will read the 4-byte length prefix,
   * then read the full message payload (ID + data).
   *
   * @return A PeerMessage struct.
   * @throws std::runtime_error on read failure.
   */
  PeerMessage readMessage();

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
  /**
   * @brief Sends a generic message to the peer.
   * Client doesn't use this, each message will have own function
   * Prepends the 4-byte length and 1-byte ID.
   */
  void sendMessage(uint8_t id, const std::vector<unsigned char>& payload);

  std::string ip_;
  std::string port_str_;
  tcp::socket socket_; // The socket for this connection
};

#endif // PEER_H
