#ifndef PEER_CONNECTION_H
#define PEER_CONNECTION_H

#include <boost/asio.hpp> // Main Asio header
#include <boost/asio/steady_timer.hpp>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <functional> // For std::function
#include <deque>    // For write queue
#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>

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
 * @brief Manages the raw TCP socket and byte-level communication.
 * 
 * Uses aynchronous reads and writes and communicates with peer through completion handlers.
 * 
 * Handles sending KEEP ALIVE messages every 2 minutes
 */
class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
public:
  // --- Callbacks for the Logic Layer (Peer) ---
  using HandshakeCallback = std::function<void(const boost::system::error_code&, std::vector<unsigned char>)>;
  using MessageCallback = std::function<void(const boost::system::error_code&, std::optional<PeerMessage>)>;
  
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

  virtual ~PeerConnection() = default;

  /**
   * @brief Starts the connection process for an OUTBOUND connection.
   *
   * This will connect, and send handshake
   */
  virtual void startAsOutbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    HandshakeCallback handshakeHandler,
    MessageCallback messageHandler
  );

  /**
   * @brief Starts the connection process for an INBOUND connection.
   *
   * This will receive a handshake, and reply using handshakeHandler
   */
  virtual void startAsInbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    HandshakeCallback handshakeHandler,
    MessageCallback messageHandler
  );

  /**
   * @brief Sends a generic message to the peer.
   * 
   * This does not check whether payload is correct.
   * 
   * Payload can be empty depending on the message id
   * 
   * e.g. sending a choke message (id = 1)
   * sendMessage(1, {}) // Empty payload
   * 
   * @param id id of the message to send
   * @param payload The payload of the message
   */
  virtual void sendMessage(uint8_t id, const std::vector<unsigned char>& payload);

  /**
   * @brief Sends the KEEP ALIVE message necessary
   */
  void sendKeepAlivePacket();

  /**
   * @brief Closes the connection by closing the socket and stopping the timer
   */
  virtual void close(const boost::system::error_code& ec = {});

  /**
   * @brief Gets the IP address of the remote peer (for logging).
   */
  std::string get_ip() const { return ip_; }


  /**
   * @brief Getters for Download/Upload
   */
  uint64_t getDownloadRate() const { return downloadRate_; }
  int64_t getUploadRate() const { return uploadRate_; }

private:
  // --- Inbound Handshake ---

  /**
   * The async inbound handshake has chained steps of events
   * 
   * After startAsInbound is called bytes from the peer are read.
   * These are named async.
   * 
   * The completion handler is then called which checks the message for
   * validity.
   *
   */
  void asyncReadInboundHandshake();
  void handleReadInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred);
  void asyncWriteInboundHandshake();
  void asyncWriteInboundHandshake(std::vector<unsigned char> receivedPeerId); // Overload
  void handleWriteInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred);
  void handleWriteInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred, const std::vector<unsigned char>& peerId); // Overload


  // --- Async Read Loop ---
  /**
   * Read header and body then send to peer handler
   */
  void startAsyncRead();
  void handleReadHeader(const boost::system::error_code& ec);
  void startAsyncReadBody(uint32_t msgLength);
  void handleReadBody(uint32_t msgLength, const boost::system::error_code& ec);

  // --- Async Write Loop ---
  void doWrite();
  void handleWrite(const boost::system::error_code& ec, size_t bytesTransferred);

   /**
    * This connects, resolves, and send handshake
    * 
   * The async inbound handshake has chained steps of events
   * 
   * After startAsInbound is called bytes from the peer are read.
   * These are named async.
   * 
   * The completion handler is then called which checks the message for
   * validity.
   * 
   * A timer is started when doResolve is called in start functions which
   * cancels the connection if hit to limit max time waiting for peers.
   */
  void doConnect(const tcp::endpoint& endpoint);
  void handleConnect(const boost::system::error_code& ec);
  void doWriteHandshake();
  void handleWriteHandshake(const boost::system::error_code& ec, size_t bytesTransferred);
  void doReadHandshake();
  void handleReadHandshake(const boost::system::error_code& ec, size_t bytesTransferred);
  void handleTimeout(const boost::system::error_code& ec);

  // -- Speed Tracking ---

  /**
   * @brief Starts speedTimer_
   */
  void startSpeedTimer();

  /**
   * @brief Calculates upload and download speed of peer
   */
  void calculateSpeed(const boost::system::error_code& ec);

  // --- Keep Alive ---

  /**
   * @brief Starts a timer for how long to wait until sending keep alive
   */
  void startKeepAliveTimer();
  /**
   * @brief Checks the last time we wrote a message and 
   * sends a keep alive message if it's greater than
   * the maximum time in between messages
   */
  void checkKeepAlive(const boost::system::error_code& ec);
       
  asio::steady_timer keepAliveTimer_; 
  std::chrono::steady_clock::time_point lastWriteTime_;

  // --- Download/Upload information
  asio::steady_timer speedTimer_;
  uint64_t downloadRate_ = 0;
  uint64_t uploadRate_ = 0;
  uint64_t bytesDownloadedInterval_ = 0;
  uint64_t bytesUploadedInterval_ = 0;

  // --- Asio state for connections ---
  std::string ip_;
  std::string port_str_;
  tcp::socket socket_;
  tcp::resolver resolver_;
  boost::asio::steady_timer timer_;

  // --- Buffers ---
  std::vector<uint8_t> readHeaderBuffer_; // 4-byte length prefix
  std::vector<uint8_t> readBodyBuffer_;   // For message ID + payload
  std::vector<uint8_t> handshakeBuffer_;  // 68 bytes

  // --- Write Queue ---
  std::deque<std::vector<uint8_t>> writeQueue_;

  // --- Torrent Info (for handshake) ---
  std::vector<unsigned char> infoHash_; 
  std::string peerId_;

  // --- Callbacks ---
  HandshakeCallback handshakeHandler_;
  MessageCallback messageHandler_;
};

#endif // PEER_CONNECTION_H