#include "peer.h"
#include <iostream>
#include <stdexcept>
#include <cstring> // For memcpy
#include <openssl/sha.h>

// CONSTANTS

// Standard block size
static constexpr uint32_t BLOCK_SIZE = 16384; // 2^14 16KB

/**
 * @brief Constructor for OUTBOUND connections (client initiates).
 */
PeerConnection::PeerConnection(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port)
  : ip_(std::move(peer_ip)),
    port_str_(std::to_string(peer_port)),
    socket_(io_context), // Initialize the socket with the io_context
    readHeaderBuffer_(4),
    handshakeBuffer_(68)
{
}

/**
 * @brief Constructor for INBOUND connections (they initiate).
 */
PeerConnection::PeerConnection(asio::io_context& io_context, tcp::socket socket)
  : std::enable_shared_from_this<PeerConnection>(),
    socket_(std::move(socket)), // Move the already-connected socket
    readHeaderBuffer_(4),
    handshakeBuffer_(68)
{
  try {
    // Get the IP and port from the socket
    ip_ = socket_.remote_endpoint().address().to_string();
    port_str_ = std::to_string(socket_.remote_endpoint().port());
  } catch (const std::exception& e) {
    std::cerr << "Error getting remote endpoint: " << e.what() << std::endl;
    ip_ = "unknown";
    port_str_ = "0";
  }
}

/**
 * @brief Performs the 68-byte BitTorrent handshake.
 * @return The 20-byte peer_id from the other client, or an empty vector on failure.
 */
std::vector<unsigned char> PeerConnection::performHandshake() {
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
  std::vector<unsigned char> handshakeMsg(68);
  
  // pstrlen
  handshakeMsg[0] = 19;
  
  // pstr ("BitTorrent protocol")
  // In version 1.0 of the BitTorrent protocol,
  // pstrlen = 19, and pstr = "BitTorrent protocol".
  const char* pstr = "BitTorrent protocol";
  memcpy(&handshakeMsg[1], pstr, 19);

  // reserved (8 bytes of 0)
  // std::vector initializes to 0, so this is already done.

  // info_hash (20 bytes)
  memcpy(&handshakeMsg[28], infoHash_.data(), 20);

  // peer_id (20 bytes)
  memcpy(&handshakeMsg[48], peerId_.c_str(), 20);



  // Send the handshake
  std::cout << "Sending handshake to " << ip_ << "..." << std::endl;
  try {
    asio::write(socket_, asio::buffer(handshakeMsg));
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to send handshake: " + std::string(e.what()));
  }

  // Receive the response (must also be 68 bytes)
  std::vector<unsigned char> response(68);
  try {
    size_t bytesRead = asio::read(socket_, asio::buffer(response), asio::transfer_exactly(68));
    if (bytesRead != 68) {
       throw std::runtime_error("Peer did not send full 68-byte handshake.");
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to read handshake: " + std::string(e.what()));
  }

  // Validate the response
  // Should be "BitTorrent protocol"
  if (memcmp(&response[1], pstr, 19) != 0) {
    throw std::runtime_error("Peer sent invalid protocol string.");
  }

  // Check info_hash
  // Should be same as clients
  if (memcmp(&response[28], infoHash_.data(), 20) != 0) {
    throw std::runtime_error("Peer sent wrong info_hash!");
  }

  // Return the peer's ID
  std::cout << "Handshake successful with " << ip_ << "!" << std::endl;
  std::vector<unsigned char> responsePeerId(20);
  memcpy(responsePeerId.data(), &response[48], 20);
  return responsePeerId;
}


void PeerConnection::sendInterested() {
  sendMessage(2, {}); // ID 2 = interested, no payload
}

void PeerConnection::sendRequest(uint32_t pieceIndex, uint32_t begin, uint32_t length) {
  std::cout << "Sending REQUEST for piece " << pieceIndex 
            << " (begin: " << begin << ", length: " << length << ")" << std::endl;
            
  // Payload is 12 bytes: index, begin, length
  std::vector<uint8_t> payload(12);
  
  // Convert to network byte order
  uint32_t index_net = htonl(pieceIndex);
  uint32_t begin_net = htonl(begin);
  uint32_t length_net = htonl(length);
  
  memcpy(&payload[0], &index_net, 4);
  memcpy(&payload[4], &begin_net, 4);
  memcpy(&payload[8], &length_net, 4);
  
  sendMessage(6, payload); // ID 6 = request
}

// --- STARTUP LOGIC (OUTBOUND) ---
  /**
   * @brief Starts the connection process for an OUTBOUND connection.
   * (A connection the client makes from the peer list)
   *
   * This will connect, handshake, send bitfield, and start the message loop.
   */
void PeerConnection::startAsOutbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    long long pieceLength, 
    long long totalLength, 
    size_t numPieces, 
    std::vector<uint8_t>* myBitfield,
    std::string* pieceHashes
) {
  // Store torrent info
  infoHash_ = infoHash;
  peerId_ = peerId;
  pieceLength_ = pieceLength;
  totalLength_ = totalLength;
  numPieces_ = numPieces;
  myBitfield_ = myBitfield;
  pieceHashes_ = pieceHashes;
  
  try {
    
    // Connect via socket
    connect(); 

    performHandshake();

    sendBitfield(*myBitfield_);
    
    // Start async message loop
    startAsyncRead();

  } catch (const std::exception& e) {
      std::cerr << "  Failed to start outbound connection to " << ip_ << ": " << e.what() << std::endl;
  }
}

/**
 * @brief Constructs a peer connection from ip and port
 */
void PeerConnection::connect() {
  try {
    // Resolve the IP and Port
    // This converts the string IP/port into a list of endpoints
    tcp::resolver resolver(socket_.get_executor());
    auto endpoints = resolver.resolve(ip_, port_str_);

    // Connect to the first resolved endpoint
    // This is a synchronous connect call. It will block until
    // it connects or times out.
    asio::connect(socket_, endpoints);

    std::cout << "Successfully connected to peer: " << ip_ << ":" << port_str_ << std::endl;

  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to connect to peer " + ip_ + ":" + port_str_ + ": " + e.what());
  }
}

/**
 * @brief Constructs and sends a Bitfield message (ID=5) to the peer.
 * @param bitfield The raw bytes of the bitfield.
 * @throws std::runtime_error on send failure.
 */
void PeerConnection::sendBitfield(const std::vector<uint8_t>& bitfield) {
  // A bitfield message has ID = 5
  std::vector<unsigned char> payload(bitfield.begin(), bitfield.end());
  sendMessage(5, payload);
  std::cout << "Sent bitfield (" << payload.size() << " bytes) to " << ip_ << std::endl;
}

// --- STARTUP LOGIC (INBOUND) ---

void PeerConnection::startAsInbound(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    long long pieceLength, 
    long long totalLength, 
    size_t numPieces, 
    std::vector<uint8_t>* myBitfield,
    std::string* pieceHashes
) {
  // Store torrent info
  infoHash_ = infoHash;
  peerId_ = peerId;
  pieceLength_ = pieceLength;
  totalLength_ = totalLength;
  numPieces_ = numPieces;
  myBitfield_ = myBitfield;
  pieceHashes_ = pieceHashes;

  // Read peer connecting handshake
  asyncReadInboundHandshake();
}

/**
 * @brief (INBOUND) 1. Start reading the 68-byte handshake.
 */
void PeerConnection::asyncReadInboundHandshake() {
  auto self = shared_from_this();
  asio::async_read(socket_, asio::buffer(handshakeBuffer_), asio::transfer_exactly(68),
    [this, self](const boost::system::error_code& ec, size_t bytesTransferred) {
      handleReadInboundHandshake(ec, bytesTransferred);
    });
}

/**
 * @brief (INBOUND) 2. Handle the 68-byte handshake we received.
 */
void PeerConnection::handleReadInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Inbound handshake read error: " << ec.message() << std::endl;
    return; // Close connection
  }
  if (bytesTransferred != 68) {
     std::cerr << "[" << ip_ << "] Inbound handshake read incomplete." << std::endl;
     return; // Close
  }
  
  // Validate the handshake
  // Should be "BitTorrent protocol"
  const char* pstr = "BitTorrent protocol";
  if (memcmp(&handshakeBuffer_[1], pstr, 19) != 0) {
    std::cerr << "[" << ip_ << "] Inbound handshake invalid protocol." << std::endl;
    return; // Close
  }

  // Check info_hash
  // Should be same as clients
  if (memcmp(&handshakeBuffer_[28], infoHash_.data(), 20) != 0) {
    std::cerr << "[" << ip_ << "] Inbound handshake wrong info_hash." << std::endl;
    return; // Close
  }
  
  std::cout << "[" << ip_ << "] Inbound handshake validated." << std::endl;
  // TODO: Store their peer ID from handshakeBuffer_[48]
  
  // Send handshake in reply
  asyncWriteInboundHandshake();
}

/**
 * @brief (INBOUND) 3. Send our 68-byte handshake reply.
 */
void PeerConnection::asyncWriteInboundHandshake() {
  // Create our handshake
  handshakeBuffer_.resize(68); // Reuse the buffer
  handshakeBuffer_[0] = 19;
  const char* pstr = "BitTorrent protocol";
  memcpy(&handshakeBuffer_[1], pstr, 19);
  memcpy(&handshakeBuffer_[28], infoHash_.data(), 20);
  memcpy(&handshakeBuffer_[48], peerId_.c_str(), 20);

  auto self = shared_from_this();
  asio::async_write(socket_, asio::buffer(handshakeBuffer_),
    [this, self](const boost::system::error_code& ec, size_t bytesTransferred) {
      handleWriteInboundHandshake(ec, bytesTransferred);
    });
}

/**
 * @brief (INBOUND) 4. Handle the completion of our handshake write.
 */
void PeerConnection::handleWriteInboundHandshake(const boost::system::error_code& ec, size_t bytesTransferred) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Inbound handshake write error: " << ec.message() << std::endl;
    return;
  }
  
  std::cout << "[" << ip_ << "] Replied with our handshake." << std::endl;

  // Send our bitfield
  asyncWriteInboundBitfield();
}

/**
 * @brief (INBOUND) Send our bitfield.
 */
void PeerConnection::asyncWriteInboundBitfield() {
  // Create message: <len><id><payload>
  uint32_t length = 1 + myBitfield_->size();
  uint32_t length_net = htonl(length);
  uint8_t id = 5;

  // We need to use a stable buffer for async write, so we'll
  // build the message in readBodyBuffer_ (re-using it)
  readBodyBuffer_.resize(4 + length);
  memcpy(&readBodyBuffer_[0], &length_net, 4);
  memcpy(&readBodyBuffer_[4], &id, 1);
  memcpy(&readBodyBuffer_[5], myBitfield_->data(), myBitfield_->size());

  auto self = shared_from_this();
  asio::async_write(socket_, asio::buffer(readBodyBuffer_),
    [this, self](const boost::system::error_code& ec, size_t bytesTransferred) {
      handleWriteInboundBitfield(ec, bytesTransferred);
    });
}

/**
 * @brief (INBOUND) 6. Handle completion of bitfield write.
 */
void PeerConnection::handleWriteInboundBitfield(const boost::system::error_code& ec, size_t bytesTransferred) {
   if (ec) {
    std::cerr << "[" << ip_ << "] Inbound bitfield write error: " << ec.message() << std::endl;
    return;
  }
  
  std::cout << "[" << ip_ << "] Sent bitfield to inbound peer." << std::endl;
  
  // Start the main message loop
  startAsyncRead();
}

/**
 * @brief Sends a generic message to the peer.
 * Client doesn't use this, each message will have own function
 * Prepends the 4-byte length and 1-byte ID.
 */
void PeerConnection::sendMessage(uint8_t id, const std::vector<unsigned char>& payload) {
  try {
    // Calculate length
    // 1 byte for ID + payload size
    uint32_t length = 1 + payload.size();
    uint32_t length_net = htonl(length); // Convert to network byte order

    // Create a buffer to send
    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(&length_net, 4));
    buffers.push_back(asio::buffer(&id, 1));
    if (!payload.empty()) {
      buffers.push_back(asio::buffer(payload));
    }

    // Send the composed message
    asio::write(socket_, buffers);

  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to send message: " + std::string(e.what()));
  }
}

// --- Async Read Loop ---

/**
 * @brief Begins the asyncronous read for the 4-byte message header
 * 
 * Calls handleReadHeader as the completion handler
 */
void PeerConnection::startAsyncRead() {
  asio::async_read(socket_, asio::buffer(readHeaderBuffer_), asio::transfer_exactly(4),
    [this](const boost::system::error_code& ec, size_t /*bytesTransferred*/) {
      handleReadHeader(ec);
    });
}

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
void PeerConnection::handleReadHeader(const boost::system::error_code& ec) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Error reading header: " << ec.message() << std::endl;
    return; // Stop the loop
  }

  uint32_t msgLength_net;
  memcpy(&msgLength_net, readHeaderBuffer_.data(), 4);
  uint32_t msgLength = ntohl(msgLength_net);

  if (msgLength == 0) {
    // Keep-alive message
    std::cout << "[" << ip_ << "] Received Keep-Alive" << std::endl;
    // Just start the next read
    startAsyncRead();
  } else if (msgLength > 200000) { // Sanity check (e.g., > 1.5 * (16k block + headers))
      std::cerr << "[" << ip_ << "] Error: Message length too large: " << msgLength << std::endl;
      return; // Stop
  } else {
    // Resize the body buffer and start reading the body
    readBodyBuffer_.resize(msgLength);
    startAsyncReadBody(msgLength);
  }
}

/**
 * @brief Begins an asynchronous read for the message body (ID + payload).
 * 
 * Calls handleReadBody as the completion handler
 * 
 * @param msgLength The length of the body to read.
 */
void PeerConnection::startAsyncReadBody(uint32_t msgLength) {
  asio::async_read(socket_, asio::buffer(readBodyBuffer_), asio::transfer_exactly(msgLength),
    [this, msgLength](const boost::system::error_code& ec, size_t /*bytesTransferred*/) {
      handleReadBody(msgLength, ec);
    });
}

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
void PeerConnection::handleReadBody(uint32_t msgLength, const boost::system::error_code& ec) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Error reading body: " << ec.message() << std::endl;
    return; // Stop the loop
  }

  PeerMessage msg;
  msg.id = readBodyBuffer_[0]; // First byte is ID
  msg.payload.assign(readBodyBuffer_.begin() + 1, readBodyBuffer_.end()); // Rest is payload

  // Handle the message (Update state)
  handleMessage(std::move(msg));

  // Do an action (Make decisions)
  doAction();

  // Continue the loop
  startAsyncRead();
}

/**
 * @brief Main message router
 * @param peer The peer that sent the message.
 * @param msg The message received from the peer.
 */
void PeerConnection::handleMessage(PeerMessage msg) {
  switch (msg.id) {
    // choke: <len=0001><id=0>
    case 0: handleChoke(); break;
    
    // unchoke: <len=0001><id=1>
    case 1: handleUnchoke(); break;

    // have: <len=0005><id=4><piece index>
    case 4: handleHave(msg); break;

    // bitfield: <len=0001+X><id=5><bitfield>
    case 5: handleBitfield(msg); break;

    // piece: <len=0009+X><id=7><index><begin><block>
    case 7: handlePiece(msg); break;
    default:
      std::cout << "[" << ip_ << "] Received unhandled message. ID: " << (int)msg.id << std::endl;
  }
}

// --- Message Actions (State Act) ---
// This should be fleshed out more to emcompass
// an entire state machine of a peer

/**
 * @brief Makes decisions based on the current peer state.
 */
void PeerConnection::doAction() {
  // Action 1: Check if we should be interested in this peer.
  checkAndSendInterested();

  // Action 2: Check if we can and should request pieces (pipelining).
  if (amInterested_ && !peerChoking_) {
    // We are interested, and the peer is not choking us.
    // Fill the request pipeline.
    requestPiece();
  }
}

/**
 * @brief Fills the request pipeline using the "First Available" strategy.
 *
 * It finds the first piece the peer has that we don't,
 * and then requests all blocks for that piece.
 * 
 * @todo Should be using "Rarest first" but that requires 
 * coordinating nodes
 */
void PeerConnection::requestPiece() {

  // If the bitfield has not been updated since last failed search
  // We have searched through the pieces and not found any match
  if (!bitfieldUpdated_) {
    std::cout << "[" << ip_ << "] All pieces checked/downloaded, nothing to request." << std::endl;
    return;
  }

  // Fill the pipeline up to MAX_PIPELINE_SIZE
  while (inFlightRequests_.size() < MAX_PIPELINE_SIZE) {
    
    // If we're not working on a piece (offset is 0),
    // we must find the next available piece to start.
    if (nextBlockOffset_ == 0) {

      bool foundPiece = false;
      // Search bitfield to find a piece peer has but client doesn't
      for (size_t i = 0; i < numPieces_; ++i) {
        if (hasPiece(i) && !clientHasPiece(i)) {
          // Found piece to request
          nextPieceIndex_ = i;
          foundPiece = true;
          std::cout << "[" << ip_ << "] Found new piece to download: " << nextPieceIndex_ << std::endl;

          // Calculate the true length of this piece
          /**
           * New length is size of piece 16KB
           * 
           * UNLESS
           * 
           * It's last piece in which it is
           * totalLength_ - totalDownloaded
           */
          long long thisPieceLength = pieceLength_;
          long long totalDownloaded = (long long)nextPieceIndex_ * pieceLength_;
          if (totalDownloaded + thisPieceLength > totalLength_) {
              thisPieceLength = totalLength_ - totalDownloaded;
          }
          currentPieceBuffer_.resize(thisPieceLength);
          break;
        }
      }

      // If we still haven't found a piece, this peer has nothing
      // for us.
      if (!foundPiece) {
        // We've searched to the end and found nothing.
        // We set bitfieldUpdated_ to false to not search again
        bitfieldUpdated_ = false;
        std::cout << "[" << ip_ << "] Peer has no pieces we need (from " 
                  << nextPieceIndex_ << " onwards). Waiting." << std::endl;
        return; // Exit request
      }
    }

    // If we have requested all blocks for this piece
    if (nextBlockOffset_ >= currentPieceBuffer_.size()) {
      // Update bitfield searched flag
      bitfieldUpdated_ = false;
      return;
    }

    long long thisPieceLength = currentPieceBuffer_.size();
    if (thisPieceLength == 0) {
        // This can happen if we found a piece but failed to resize buffer
        // Reset and attempt to request the first block
        nextBlockOffset_ = 0;
        return;
    }

    // If we're here, nextPieceIndex_ is set to a piece that
    // the peer has and we don't.
    
    // Client have a piece to download
    uint32_t blockLength = BLOCK_SIZE;
    
    // TODO: Need to handle the last piece/block, which might be shorter.
    if (nextBlockOffset_ + BLOCK_SIZE > pieceLength_) {
        blockLength = pieceLength_ - nextBlockOffset_;
    }
    
    // Send the request
    std::cout << "[" << ip_ << "] --- ACTION: Requesting piece " << nextPieceIndex_ 
              << ", Block offset " << nextBlockOffset_ << " ---" << std::endl;
              
    sendRequest(
        static_cast<uint32_t>(nextPieceIndex_), // pieceIndex
        nextBlockOffset_,                      // begin
        blockLength                            // length
    );

    // Add this to our in-flight list
    inFlightRequests_.push_back(PendingRequest{
        static_cast<uint32_t>(nextPieceIndex_),
        nextBlockOffset_,
        blockLength
    });

    // Advance to the next block
    nextBlockOffset_ += blockLength;
    
  }
}

/**
 * @brief Checks if we are interested in the peer and sends an
 * Interested message (ID 2) if we aren't already.
 */
void PeerConnection::checkAndSendInterested() {
  if (amInterested_) {
    return; // Already interested
  }

  for (size_t i = 0; i < numPieces_; ++i) {
    if (hasPiece(i) && !clientHasPiece(i)) {
      std::cout << "[" << ip_ << "] Peer has piece " << i << " which we don't. Sending INTERESTED." << std::endl;
      sendInterested();
      amInterested_ = true;
      return; 
    }
  }
}


// --- Message Handlers (State Updaters) ---

/**
 * @brief Handles a choke message from a peer
 * 
 * Updates the requests buffer to be empty
 * AND
 * Sets peerChoking_ to true
 * 
 * A choke message means a peer will not accept any messages from us.
 * 
 * Any existing messages should be considered to be discarded
 */
void PeerConnection::handleChoke() {
  std::cout << "[" << ip_ << "] Received CHOKE" << std::endl;
  peerChoking_ = true;

  // If we had requests pending, they are now dead.
  // A real client might re-queue these.
  // For now, we just clear them.
  if (!inFlightRequests_.empty()) {
      std::cout << "  Peer choked us, clearing " << inFlightRequests_.size() << " in-flight requests." << std::endl;
      inFlightRequests_.clear();
      
      // We must reset our download position to the start
      // of the piece we were working on.
      nextPieceIndex_ = inFlightRequests_[0].pieceIndex;
      nextBlockOffset_ = inFlightRequests_[0].begin;
  }
}

/**
 * @brief Handles an unchoke message from a peer
 * 
 * Sets peerChoking_ to false
 * 
 * An unchoke message means a peer is ready to recieve messages
 */
void PeerConnection::handleUnchoke() {
  std::cout << "[" << ip_ << "] Received UNCHOKE" << std::endl;
  peerChoking_ = false;
}

/**
 * @brief Handles a have message from a peer
 * 
 * Updates bitfield of peer
 * 
 * A have message means this peer has this signified piece
 */
void PeerConnection::handleHave(const PeerMessage& msg) {
  if (msg.payload.size() != 4) {
    std::cerr << "[" << ip_ << "] Invalid HAVE message payload size: " << msg.payload.size() << std::endl;
    return;
  }
  
  uint32_t pieceIndex;
  memcpy(&pieceIndex, msg.payload.data(), 4);
  pieceIndex = ntohl(pieceIndex);

  std::cout << "[" << ip_ << "] Received HAVE for piece " << pieceIndex << std::endl;
  
  // Set piece in bitfield
  setHavePiece(pieceIndex);
}

/**
 * @brief Handles a have bitfield from a peer
 * 
 * Updates bitfield of peer
 * 
 * A have message means this peer has this signified piece
 */
void PeerConnection::handleBitfield(const PeerMessage& msg) {
  std::cout << "[" << ip_ << "] Received BITFIELD (" << msg.payload.size() << " bytes)" << std::endl;
  bitfield_ = msg.payload;
  bitfieldUpdated_ = true;
}


/**
 * @brief Updates the peer's bitfield to indicate they have a piece.
 * @param pieceIndex The zero-based index of the piece.
 */
void PeerConnection::setHavePiece(uint32_t pieceIndex) {
  size_t byte_index = pieceIndex / 8;
  uint8_t bit_index = 7 - (pieceIndex % 8); // 7 - ... because bits are 7..0

  // Ensure the bitfield is large enough.
  // This is crucial if we get a HAVE message before a BITFIELD message.
  if (byte_index >= bitfield_.size()) {
    bitfield_.resize(byte_index + 1, 0); // Resize and fill with 0s
  }

  // Set the bit
  bitfield_[byte_index] |= (1 << bit_index);

  // Update bitfield search
  bitfieldUpdated_ = true;
}

/**
 * @brief Checks if the peer has a specific piece, based on their bitfield.
 * @param pieceIndex The zero-based index of the piece.
 * @return True if the peer has the piece, false otherwise.
 */
bool PeerConnection::hasPiece(uint32_t pieceIndex) const {
  //
  size_t byte_index = pieceIndex / 8;
  uint8_t bit_index = 7 - (pieceIndex % 8);

  // If the piece index is out of bounds of their bitfield, they don't have it
  if (byte_index >= bitfield_.size()) {
    return false;
  }

  // Check if the bit is set
  return (bitfield_[byte_index] & (1 << bit_index)) != 0;
}

void PeerConnection::handlePiece(const PeerMessage& msg) {
    if (msg.payload.size() < 8) {
        std::cerr << "[" << ip_ << "] Invalid PIECE message payload size: " << msg.payload.size() << std::endl;
        return;
    }

    // Copy data and convert to host byte order
    uint32_t pieceIndex, begin;
    memcpy(&pieceIndex, msg.payload.data(), 4);
    memcpy(&begin, msg.payload.data() + 4, 4);
    pieceIndex = ntohl(pieceIndex);
    begin = ntohl(begin);
    size_t blockLength = msg.payload.size() - 8;


    std::cout << "[" << ip_ << "] Received PIECE: Index=" << pieceIndex 
              << ", Begin=" << begin
              << ", Length=" << blockLength << std::endl;

    // Find this block from our in-flight list
    // @todo change inFlightRequests to requestsBuffer
    auto it = std::find_if(inFlightRequests_.begin(), inFlightRequests_.end(), 
        [pieceIndex, begin, blockLength](const PendingRequest& req) {
            return req.pieceIndex == pieceIndex && 
                   req.begin == begin && 
                   req.length == blockLength;
        });

    if (it != inFlightRequests_.end()) {
      // Remove from buffer
      inFlightRequests_.erase(it);

      // Save the block data into our piece buffer
      if (pieceIndex == nextPieceIndex_ && (begin + blockLength) <= currentPieceBuffer_.size()) {
          const unsigned char* blockData = msg.payload.data() + 8;
          memcpy(&currentPieceBuffer_[begin], blockData, blockLength);
          std::cout << "  Saved " << blockLength << " bytes to piece buffer." << std::endl;
      } else {
          std::cout << "  WARNING: Received piece data for wrong piece/offset. Discarding." << std::endl;
          return;
      }

      // Check if this piece is now complete
      // (We are done if the pipeline for this piece is empty
      // AND our offset is at the end)
      bool pieceFinished = (nextBlockOffset_ >= currentPieceBuffer_.size());
      
      // Check if any other in-flight requests are for this piece
      for (const auto& req : inFlightRequests_) {
          if (req.pieceIndex == nextPieceIndex_) {
              pieceFinished = false; // Still waiting for more blocks
              break;
          }
      }

      if (pieceFinished) {
          std::cout << "  COMPLETED PIECE " << pieceIndex << " (all blocks received)!" << std::endl;
          
          // Verify hash
          if (verifyPieceHash(pieceIndex)) {
              std::cout << "  HASH OK for piece " << pieceIndex << "!" << std::endl;
              
              // Set client bitfield
              size_t my_byte_index = pieceIndex / 8;
              uint8_t my_bit_index = 7 - (pieceIndex % 8);
              if (myBitfield_) {
                  // @TODO: This should be thread-safe if we add multi-threading
                  (*myBitfield_)[my_byte_index] |= (1 << my_bit_index);
              }
              
              // @TODO: Write piece to file
              
              // Advance to next piece
              nextBlockOffset_ = 0;
              nextPieceIndex_++;
              currentPieceBuffer_.clear();

          } else {
              std::cout << "  *** HASH FAILED for piece " << pieceIndex << " ***" << std::endl;
              // Discard data and reset to re-download this piece
              nextBlockOffset_ = 0;
              // nextPieceIndex_ remains the same
              currentPieceBuffer_.clear();
          }
        }

    } else {
        std::cout << "  WARNING: Received a PIECE that doesn't match any request." << std::endl;
    }
}


// --- Helper Functions ---

/**
 * @brief Checks if *we* have a specific piece.
 */
bool PeerConnection::clientHasPiece(size_t pieceIndex) const {
    if (!myBitfield_) return false; // Not initialized

    size_t byte_index = pieceIndex / 8;
    uint8_t bit_index = 7 - (pieceIndex % 8);

    if (byte_index >= myBitfield_->size()) {
        return false; // Should not happen if initialized
    }
    return ((*myBitfield_)[byte_index] & (1 << bit_index)) != 0;
}

/**
 * @brief Verifies the SHA-1 hash of the piece in currentPieceBuffer_.
 */
bool PeerConnection::verifyPieceHash(size_t pieceIndex) {
    if (!pieceHashes_) {
        std::cerr << "[" << ip_ << "] ERROR: Cannot verify hash, no hashes pointer." << std::endl;
        return false;
    }
    if (currentPieceBuffer_.empty()) {
        std::cerr << "[" << ip_ << "] ERROR: Cannot verify hash, piece buffer is empty." << std::endl;
        return false;
    }
    
    // Calculate the hash
    unsigned char calculatedHash[SHA_DIGEST_LENGTH]; // 20 bytes
    SHA1(currentPieceBuffer_.data(), currentPieceBuffer_.size(), calculatedHash);
    
    // Get the expected hash
    const char* expectedHash = pieceHashes_->data() + (pieceIndex * 20);
    
    // Compare
    return memcmp(calculatedHash, expectedHash, SHA_DIGEST_LENGTH) == 0;
}