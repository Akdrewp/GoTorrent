#include "peer.h"
#include <iostream>
#include <stdexcept>
#include <cstring> // For memcpy
#include <openssl/sha.h>

// CONSTANTS

// Standard block size
static constexpr uint32_t BLOCK_SIZE = 16384; // 2^14 16KB

/**
 * @brief Constructor for OUTBOUND connections.
 */
Peer::Peer(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port)
  : ip_(peer_ip),
    conn_(std::make_shared<PeerConnection>(io_context, peer_ip, peer_port))
{
}

/**
 * @brief Constructor for INBOUND connections.
 */
Peer::Peer(asio::io_context& io_context, tcp::socket socket)
  : ip_(socket.remote_endpoint().address().to_string()),
    conn_(std::make_shared<PeerConnection>(io_context, std::move(socket)))
{
}

// --- STARTUP LOGIC ---

/**
 * @brief Starts the connection process for an OUTBOUND connection.
 *
 * This will connect, handshake, send bitfield, and start the message loop.
 */
void Peer::startAsOutbound(
  const std::vector<unsigned char>& infoHash,
  const std::string& peerId,
  long long pieceLength, 
  long long totalLength, 
  size_t numPieces, 
  std::vector<uint8_t>* myBitfield,
  std::string* pieceHashes
) {
  // Store torrent info
  pieceLength_ = pieceLength;
  totalLength_ = totalLength;
  numPieces_ = numPieces;
  myBitfield_ = myBitfield;
  pieceHashes_ = pieceHashes;
  
  // Create 'this' binding for callbacks
  auto self = shared_from_this();
  
  // Start the TRANSPORT layer's connection process
  conn_->startAsOutbound(
    infoHash,
    peerId,
    // Handshake callback
    [this, self](const boost::system::error_code& ec, std::vector<unsigned char> peerId) {
      onHandshakeComplete(ec, std::move(peerId));
    },
    // Message callback
    [this, self](const boost::system::error_code& ec, std::optional<PeerMessage> msg) {
      onMessageReceived(ec, std::move(msg));
    }
  );
}

/**
 * @brief Starts the connection process for an INBOUND connection.
 *
 * This will connect, recieve and verify handshake, send bitfield, and start the message loop.
 */
void Peer::startAsInbound(
  const std::vector<unsigned char>& infoHash,
  const std::string& peerId,
  long long pieceLength, 
  long long totalLength, 
  size_t numPieces, 
  std::vector<uint8_t>* myBitfield,
  std::string* pieceHashes
) {
  // Store torrent info
  pieceLength_ = pieceLength;
  totalLength_ = totalLength;
  numPieces_ = numPieces;
  myBitfield_ = myBitfield;
  pieceHashes_ = pieceHashes;

  auto self = shared_from_this();
  conn_->startAsInbound(
    infoHash,
    peerId,
    [this, self](const boost::system::error_code& ec, std::vector<unsigned char> peerId) {
      onHandshakeComplete(ec, std::move(peerId));
    },
    [this, self](const boost::system::error_code& ec, std::optional<PeerMessage> msg) {
      onMessageReceived(ec, std::move(msg));
    }
  );
}


// --- Callback handlers for PeerConnection ---

void Peer::onHandshakeComplete(const boost::system::error_code& ec, std::vector<unsigned char> peerId) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Logic: Handshake failed." << std::endl;
    // Connection is already closed by PeerConnection. We just stop.
    return;
  }

  std::cout << "[" << ip_ << "] Logic: Handshake complete. Sending bitfield." << std::endl;
  // @TODO: Store their peerId
  
  // Now that handshake is done, send our bitfield.
  sendBitfield();
}

void Peer::onMessageReceived(const boost::system::error_code& ec, std::optional<PeerMessage> msg) {
  if (ec) {
    std::cerr << "[" << ip_ << "] Logic: Disconnected (" << ec.message() << ")" << std::endl;
    // We are disconnected stop
    return;
  }

  if (msg) {
    // Handle the message (Update state)
    handleMessage(std::move(*msg));

    // Do an action based on new state
    doAction();
  }
}

// --- Message Senders ---

void Peer::sendBitfield() {
  std::vector<unsigned char> payload(myBitfield_->begin(), myBitfield_->end());
  conn_->sendMessage(5, payload);
  std::cout << "[" << ip_ << "] Sent bitfield (" << payload.size() << " bytes)" << std::endl;
}

void Peer::sendInterested() {
  conn_->sendMessage(2, {}); // ID 2 = interested, no payload
}

void Peer::sendRequest(uint32_t pieceIndex, uint32_t begin, uint32_t length) {
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
  
  conn_->sendMessage(6, payload); // ID 6 = request
}

// --- Message Actions (State Act) ---
// This should be fleshed out more to emcompass
// an entire state machine of a peer

/**
 * @brief Makes decisions based on the current peer state.
 */
void Peer::doAction() {
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
void Peer::requestPiece() {

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
void Peer::checkAndSendInterested() {
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
 * @brief Main message router
 * @param peer The peer that sent the message.
 * @param msg The message received from the peer.
 */
void Peer::handleMessage(PeerMessage msg) {
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
void Peer::handleChoke() {
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
void Peer::handleUnchoke() {
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
void Peer::handleHave(const PeerMessage& msg) {
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
void Peer::handleBitfield(const PeerMessage& msg) {
  std::cout << "[" << ip_ << "] Received BITFIELD (" << msg.payload.size() << " bytes)" << std::endl;
  bitfield_ = msg.payload;
  bitfieldUpdated_ = true;
}


/**
 * @brief Updates the peer's bitfield to indicate they have a piece.
 * @param pieceIndex The zero-based index of the piece.
 */
void Peer::setHavePiece(uint32_t pieceIndex) {
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
bool Peer::hasPiece(uint32_t pieceIndex) const {
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

void Peer::handlePiece(const PeerMessage& msg) {
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
bool Peer::clientHasPiece(size_t pieceIndex) const {
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
bool Peer::verifyPieceHash(size_t pieceIndex) {
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