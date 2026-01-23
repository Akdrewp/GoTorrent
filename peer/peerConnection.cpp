#include "peerConnection.h"
#include <iostream>
#include <stdexcept>
#include <cstring> // For memcpy
#include <chrono> 
#include <boost/asio/buffer.hpp> // For asio::buffer
#include <boost/asio/steady_timer.hpp> 

static constexpr uint32_t BLOCK_SIZE = 16384; // 2^14 16KB
static constexpr int TIMEOUT_TIME = 5; // Should be 5 seconds

// --- OUTBOUND ---
PeerConnection::PeerConnection(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port)
  : ip_(std::move(peer_ip)),
    port_str_(std::to_string(peer_port)),
    socket_(io_context),
    resolver_(io_context),
    timer_(io_context),
    keepAliveTimer_(io_context),
    speedTimer_(io_context),
    readHeaderBuffer_(4),
    handshakeBuffer_(68)
{
}

// --- INBOUND ---
PeerConnection::PeerConnection(asio::io_context& io_context, tcp::socket socket)
  : socket_(std::move(socket)),
    resolver_(io_context),
    timer_(io_context),
    keepAliveTimer_(io_context),
    speedTimer_(io_context),
    readHeaderBuffer_(4),
    handshakeBuffer_(68)
{
  try {
    ip_ = socket_.remote_endpoint().address().to_string();
    port_str_ = std::to_string(socket_.remote_endpoint().port());
  } catch (const std::exception& e) {
    std::cerr << "Error getting remote endpoint: " << e.what() << std::endl;
    ip_ = "unknown";
    port_str_ = "0";
  }
}
// --- Speed Tracking ---

// --- SPEED TRACKING LOGIC ---

void PeerConnection::startSpeedTimer() {
  speedTimer_.expires_after(std::chrono::seconds(1));
  auto self = shared_from_this();
  speedTimer_.async_wait([this, self](const boost::system::error_code& ec) {
      calculateSpeed(ec);
  });
}

/**
 * @brief Updates the upload and download rate speeds
 * and starts speed timer.
 */
void PeerConnection::calculateSpeed(const boost::system::error_code& ec) {
  if (ec == asio::error::operation_aborted) return;

  // Update public rates (Bytes per second)
  downloadRate_ = bytesDownloadedInterval_;
  uploadRate_ = bytesUploadedInterval_;

  // Reset interval counters
  bytesDownloadedInterval_ = 0;
  bytesUploadedInterval_ = 0;

  // Restart timer
  startSpeedTimer();
}

// --- Keep Alive ---

/**
 * @brief Starts a keep alive timer which calls checkKeepAlive
 */
void PeerConnection::startKeepAliveTimer() {
  lastWriteTime_ = std::chrono::steady_clock::now();
  
  // 60 Seconds should be good. 
  // Reccomended is every 120 seconds without message sent.
  keepAliveTimer_.expires_after(std::chrono::seconds(60));
  
  auto self = shared_from_this();
  keepAliveTimer_.async_wait([this, self](const boost::system::error_code& ec) {
      checkKeepAlive(ec);
  });
}

void PeerConnection::checkKeepAlive(const boost::system::error_code& ec) {
  if (ec == asio::error::operation_aborted || !socket_.is_open()) return;

  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastWriteTime_).count();

  // If we haven't written anything for > 100 seconds
  // 120 seconds reccomended
  if (elapsed >= 100) {
    std::cout << "[" << ip_ << "] Idle for " << elapsed << "s. Sending Keep-Alive." << std::endl;
    sendKeepAlivePacket();
  }

  // Schedule next check
  keepAliveTimer_.expires_after(std::chrono::seconds(60));
  auto self = shared_from_this();
  keepAliveTimer_.async_wait([this, self](const boost::system::error_code& ec) {
    checkKeepAlive(ec);
  });
}

void PeerConnection::sendKeepAlivePacket() {
  std::vector<uint8_t> packet(4, 0); 
  
  writeQueue_.push_back(std::move(packet));

  if (writeQueue_.size() == 1) {
    doWrite();
  }
}

/**
 * @brief Closes the socket and cancels the timer.
 */
void PeerConnection::close(const boost::system::error_code& ec) {
  if (socket_.is_open()) {
    std::cout << "[" << ip_ << "] Closing connection." << std::endl;
    socket_.close();
    timer_.cancel();
  }
  // Inform peer disconnect
  if (messageHandler_) {
    // Call message handler with error
    auto errc = ec ? ec : boost::system::errc::make_error_code(boost::system::errc::timed_out);
    messageHandler_(errc, std::nullopt);
  }
}

// --- OUTBOUND ---

void PeerConnection::startAsOutbound(
  const std::vector<unsigned char>& infoHash,
  const std::string& peerId,
  HandshakeCallback handshakeHandler,
  MessageCallback messageHandler)
{
  // Set variables
  infoHash_ = infoHash;
  peerId_ = peerId;
  handshakeHandler_ = std::move(handshakeHandler);
  messageHandler_ = std::move(messageHandler);

  try {
    // Parse ip
    auto ip_address = asio::ip::address::from_string(ip_);
    // Create endpoint
    tcp::endpoint endpoint(ip_address, static_cast<uint16_t>(std::stoi(port_str_)));
    
    // Start connection proccess
    doConnect(endpoint);
  } catch (const std::exception& e) {
    std::cerr << "[" << ip_ << "] Invalid IP address format: " << e.what() << std::endl;
    // Invalid IP address passed
    auto self = shared_from_this();
    auto ec = boost::system::errc::make_error_code(boost::system::errc::bad_address);
    asio::post(socket_.get_executor(), [this, self, ec] {
      close(ec);
    });
  }
}

void PeerConnection::doConnect(const tcp::endpoint& endpoint) {
  auto self = shared_from_this();
  timer_.expires_after(std::chrono::seconds(TIMEOUT_TIME));
  timer_.async_wait(
    [this, self](const boost::system::error_code& ec) {
      handleTimeout(ec);
    });

  std::cout << "[" << ip_ << "] Connecting..." << std::endl;
  socket_.async_connect(endpoint,
    [this, self](const boost::system::error_code& ec)
    {
      handleConnect(ec);
    });
}

void PeerConnection::handleConnect(const boost::system::error_code& ec) {
  if (ec == asio::error::operation_aborted || !socket_.is_open()) return; // Timeout
  timer_.cancel(); // We connected (or failed), stop the timer.

  if (ec) {
    std::cerr << "[" << ip_ << "] Connect failed: " << ec.message() << std::endl;
    close(ec);
    return;
  }
  std::cout << "[" << ip_ << "] Successfully connected." << std::endl;

  startKeepAliveTimer();

  doWriteHandshake();
}

void PeerConnection::doWriteHandshake() {
  /**
   * Construct the 68-byte handshake message
   * handshake: <pstrlen><pstr><reserved><info_hash><peer_id>
   * 
   * <pstrlen (1)>
   * <pstr (19)>
   * <reserved (8)>
   * <info_hash (20)>
   * <peer_id (20)>
   * 
   * (49+len(pstr)) long
   */
  handshakeBuffer_[0] = 19;
  const char* pstr = "BitTorrent protocol";
  memcpy(&handshakeBuffer_[1], pstr, 19);
  memcpy(&handshakeBuffer_[28], infoHash_.data(), 20);
  memcpy(&handshakeBuffer_[48], peerId_.c_str(), 20);

  auto self = shared_from_this();
  std::cout << "[" << ip_ << "] Sending handshake..." << std::endl;
  asio::async_write(socket_, asio::buffer(handshakeBuffer_),
    [this, self](const boost::system::error_code& ec, size_t bytesTransferred) {
      handleWriteHandshake(ec, bytesTransferred);
    });
}

void PeerConnection::handleWriteHandshake(const boost::system::error_code& ec, size_t bytesTransferred) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Outbound handshake write error: " << ec.message() << std::endl;
    close(ec);
    return;
  }
  std::cout << "[" << ip_ << "] Sent handshake." << std::endl;
  doReadHandshake();
}

void PeerConnection::doReadHandshake() {
  auto self = shared_from_this();
  std::cout << "[" << ip_ << "] Reading handshake response..." << std::endl;
  asio::async_read(socket_, asio::buffer(handshakeBuffer_), asio::transfer_exactly(68),
    [this, self](const boost::system::error_code& ec, size_t bytesTransferred) {
      handleReadHandshake(ec, bytesTransferred);
    });
}

void PeerConnection::handleReadHandshake(const boost::system::error_code& ec, size_t bytesTransferred) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Outbound handshake read error: " << ec.message() << std::endl;
    close(ec);
    return;
  }

  // Returned handshake should be same 68 bytes
  if (bytesTransferred != 68) {
    std::cerr << "[" << ip_ << "] Outbound handshake read incomplete." << std::endl;
    close(ec);
    return;
  }

  const char* pstr = "BitTorrent protocol";
  if (memcmp(&handshakeBuffer_[1], pstr, 19) != 0) {
    std::cerr << "[" << ip_ << "] Outbound handshake invalid protocol." << std::endl;
    close(ec);
    return;
  }
  if (memcmp(&handshakeBuffer_[28], infoHash_.data(), 20) != 0) {
    std::cerr << "[" << ip_ << "] Outbound handshake wrong info_hash." << std::endl;
    close(ec);
    return;
  }
  
  std::cout << "[" << ip_ << "] Handshake successful." << std::endl;

  std::vector<unsigned char> responsePeerId(20);
  memcpy(responsePeerId.data(), &handshakeBuffer_[48], 20);
  
  // Call peer handshake handler
  handshakeHandler_({}, std::move(responsePeerId));

  // Start the main message loop
  startAsyncRead();
}

void PeerConnection::handleTimeout(const boost::system::error_code& ec) {
  if (ec == asio::error::operation_aborted) return; // Timer was cancelled.
  std::cerr << "[" << ip_ << "] *** Connection timed out after 5 seconds. ***" << std::endl;
  close(ec);
}


// --- INBOUND ---

void PeerConnection::startAsInbound(
  const std::vector<unsigned char>& infoHash,
  const std::string& peerId,
  HandshakeCallback handshakeHandler,
  MessageCallback messageHandler)
{
  infoHash_ = infoHash;
  peerId_ = peerId;
  handshakeHandler_ = std::move(handshakeHandler);
  messageHandler_ = std::move(messageHandler);
  asyncReadInboundHandshake();
}

void PeerConnection::asyncReadInboundHandshake() {
  auto self = shared_from_this();
  asio::async_read(socket_, asio::buffer(handshakeBuffer_), asio::transfer_exactly(68),
    [this, self](const boost::system::error_code& ec, size_t bytesTransferred) {
      handleReadInboundHandshake(ec, bytesTransferred);
    });
}

void PeerConnection::handleReadInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Inbound handshake read error: " << ec.message() << std::endl;
    close(ec);
    return;
  }

  // Returned handshake should be same 68 bytes
  if (bytesTransferred != 68) {
      std::cerr << "[" << ip_ << "] Inbound handshake read incomplete." << std::endl;
      close(ec);
      return;
  }
  
  const char* pstr = "BitTorrent protocol";
  if (memcmp(&handshakeBuffer_[1], pstr, 19) != 0) {
    std::cerr << "[" << ip_ << "] Inbound handshake invalid protocol." << std::endl;
    close(ec);
    return;
  }
  if (memcmp(&handshakeBuffer_[28], infoHash_.data(), 20) != 0) {
    std::cerr << "[" << ip_ << "] Inbound handshake wrong info_hash." << std::endl;
    close(ec);
    return;
  }
  
  std::cout << "[" << ip_ << "] Inbound handshake validated." << std::endl;

  // Store their peer ID
  std::vector<unsigned char> responsePeerId(20);
  memcpy(responsePeerId.data(), &handshakeBuffer_[48], 20);

  // Send handshake in reply, but pass their ID to the handler
  // so the logic layer gets it.
  asyncWriteInboundHandshake(std::move(responsePeerId));
}

void PeerConnection::asyncWriteInboundHandshake() {
  // Overload for the logic, we pass the received PeerID
  asyncWriteInboundHandshake(std::vector<unsigned char>{});
}

void PeerConnection::asyncWriteInboundHandshake(std::vector<unsigned char> receivedPeerId) {
  /**
   * Construct the 68-byte handshake message
   * handshake: <pstrlen><pstr><reserved><info_hash><peer_id>
   * 
   * <pstrlen (1)>
   * <pstr (19)>
   * <reserved (8)>
   * <info_hash (20)>
   * <peer_id (20)>
   * 
   * (49+len(pstr)) long
   */
  handshakeBuffer_.resize(68);
  handshakeBuffer_[0] = 19;
  const char* pstr = "BitTorrent protocol";
  memcpy(&handshakeBuffer_[1], pstr, 19);
  memcpy(&handshakeBuffer_[28], infoHash_.data(), 20);
  memcpy(&handshakeBuffer_[48], peerId_.c_str(), 20);

  auto self = shared_from_this();
  asio::async_write(socket_, asio::buffer(handshakeBuffer_),
    [this, self, peerId = std::move(receivedPeerId)](const boost::system::error_code& ec, size_t bytesTransferred) {
      // Pass the peerId into the completion handler
      handleWriteInboundHandshake(ec, bytesTransferred, peerId);
    });
}

void PeerConnection::handleWriteInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred) {
  // This overload is just to satisfy the old outbound chain, now unused
}

void PeerConnection::handleWriteInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred, const std::vector<unsigned char>& peerId) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Inbound handshake write error: " << ec.message() << std::endl;
    close(ec);
    return;
  }
  
  std::cout << "[" << ip_ << "] Replied with our handshake." << std::endl;

  // Handshake complete call peer handshake handler
  handshakeHandler_({}, peerId);
  
  // Start the main message loop
  startAsyncRead();
}


// --- ASYNC READ LOOP ---
/**
 * Shared READ message loop
 * 
 * Read header
 * Read body
 * Call peer message handler
 * repeat
 */

void PeerConnection::startAsyncRead() {
  auto self = shared_from_this();
  asio::async_read(socket_, asio::buffer(readHeaderBuffer_), asio::transfer_exactly(4),
    [this, self](const boost::system::error_code& ec, size_t /*bytesTransferred*/) {
      handleReadHeader(ec);
    });
}

void PeerConnection::handleReadHeader(const boost::system::error_code& ec) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Error reading header: " << ec.message() << std::endl;
    close(ec);
    return;
  }

  // Header is 4 bytes for length
  uint32_t msgLength_net;
  memcpy(&msgLength_net, readHeaderBuffer_.data(), 4);
  uint32_t msgLength = ntohl(msgLength_net);

  if (msgLength == 0) {
    std::cout << "[" << ip_ << "] Received Keep-Alive" << std::endl;
    startAsyncRead();
  } else if (msgLength > BLOCK_SIZE + 13) { // Change
    std::cerr << "[" << ip_ << "] Error: Message length too large: " << msgLength << std::endl;
    close(ec);
  } else {
    readBodyBuffer_.resize(msgLength);
    startAsyncReadBody(msgLength);
  }
}

void PeerConnection::startAsyncReadBody(uint32_t msgLength) {
  auto self = shared_from_this();
  asio::async_read(socket_, asio::buffer(readBodyBuffer_), asio::transfer_exactly(msgLength),
    [this, self, msgLength](const boost::system::error_code& ec, size_t /*bytesTransferred*/) {
      handleReadBody(msgLength, ec);
    });
}

void PeerConnection::handleReadBody(uint32_t msgLength, const boost::system::error_code& ec) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Error reading body: " << ec.message() << std::endl;
    close(ec);
    return;
  }

  PeerMessage msg;
  msg.id = readBodyBuffer_[0]; // First byte is ID
  msg.payload.assign(readBodyBuffer_.begin() + 1, readBodyBuffer_.end()); // Rest is payload

  // Send the parsed message to the LOGIC layer
  messageHandler_({}, std::move(msg));

  // Continue the loop
  startAsyncRead();
}


// --- ASYNC WRITE LOOP ---
/**
 * Shared WRITE message
 * 
 * Create the message
 * Send message
 */

void PeerConnection::sendMessage(uint8_t id, const std::vector<unsigned char>& payload) {
  uint32_t length = 1 + payload.size();
  uint32_t length_net = htonl(length);

  // Create the full message buffer
  std::vector<uint8_t> msgBuffer(4 + length);
  memcpy(&msgBuffer[0], &length_net, 4);
  memcpy(&msgBuffer[4], &id, 1);
  if (!payload.empty()) {
    memcpy(&msgBuffer[5], payload.data(), payload.size());
  }

  // Add to queue
  writeQueue_.push_back(std::move(msgBuffer));

  // If we aren't already writing, start the write loop
  if (writeQueue_.size() == 1) {
    doWrite();
  }
}

void PeerConnection::doWrite() {
  if (writeQueue_.empty()) return; // Should not happen

  auto self = shared_from_this();
  asio::async_write(socket_, asio::buffer(writeQueue_.front()),
    [this, self](const boost::system::error_code& ec, size_t bytesTransferred) {
      handleWrite(ec, bytesTransferred);
    });
}

void PeerConnection::handleWrite(const boost::system::error_code& ec, size_t bytesTransferred) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Error writing message: " << ec.message() << std::endl;
    writeQueue_.clear(); // Drop all pending messages
    close(ec);
    return;
  }

  writeQueue_.pop_front();

  lastWriteTime_ = std::chrono::steady_clock::now();

  bytesUploadedInterval_ += bytesTransferred;

  // If there's more to write, keep the loop going
  if (!writeQueue_.empty()) {
    doWrite();
  }
}