#include "peer.h"
#include "torrentSession.h" 
#include <iostream>
#include <stdexcept>
#include <cstring> // For memcpy
#include <openssl/sha.h>

// CONSTANTS

// Standard block size
static constexpr uint32_t BLOCK_SIZE = 16384; // 2^14 16KB

/**
 * @brief Constructor for a peer
 */
Peer::Peer(std::shared_ptr<PeerConnection> conn, std::string ip)
  : conn_(std::move(conn)), ip_(std::move(ip))
{
}

// --- Logging helpers ---

void Peer::log(const std::string& msg) const {
    std::cout << "[" << ip_ << "] " << msg << std::endl;
}

void Peer::logError(const std::string& msg) const {
    std::cerr << "[" << ip_ << "] " << msg << std::endl;
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
  std::weak_ptr<TorrentSession> session
) {
  // Store the session context
  session_ = session;
  
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
  std::weak_ptr<TorrentSession> session
) {
  // Store the session context
  session_ = session;

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
    logError("Logic: Handshake failed.");
    // Connection is already closed by PeerConnection. We just stop.
    return;
  }

  log("Logic: Handshake complete. Sending bitfield.");
  // @TODO: Store their peerId maybe for console logging
  
  // Now that handshake is done, send our bitfield.
  sendBitfield();
}

void Peer::onMessageReceived(const boost::system::error_code& ec, std::optional<PeerMessage> msg) {
  if (ec) {
    logError("Logic: Disconnected (" + ec.message() + ")");

    // If we were working on a piece, release the lock
    if (auto session = session_.lock()) {
      if (nextBlockOffset_ > 0 && nextBlockOffset_ < currentPieceBuffer_.size()) {
        log("Disconnected, un-assigning piece " + std::to_string(nextPieceIndex_));
        session->unassignPiece(nextPieceIndex_);
      }
    }
    return;

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
  if (auto session = session_.lock()) {
    std::vector<unsigned char> payload = session->getBitfield();
    conn_->sendMessage(5, payload); // ID 5 = bitfield, 
    log("Sent bitfield (" + std::to_string(payload.size()) + " bytes)");
  }
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
  // Check if we can and should request pieces (pipelining).
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

  // Get session
  auto session = session_.lock();
  if (!session) {
    return; // Session is gone
  }

  // Fill the pipeline up to MAX_PIPELINE_SIZE
  while (inFlightRequests_.size() < MAX_PIPELINE_SIZE) {
    
    // If we're not working on a piece (offset is 0),
    // we must find the next available piece to start.
    if (nextBlockOffset_ == 0) {
      // Get assigned piece from session
      std::optional<size_t> assignedPiece = session->assignWorkForPeer(shared_from_this());

      if (!assignedPiece) {
        // Session tell us there's no work to be done
        return; // Stop trying to request
      }

      // We have a piece

      nextPieceIndex_ = *assignedPiece;
      log("Session assigned us piece: " + std::to_string(nextPieceIndex_));

      long long pieceLength = session->getPieceLength();
      long long totalLength = session->getTotalLength();

      long long thisPieceLength = pieceLength;

      long long totalDownloaded = (long long)nextPieceIndex_ * pieceLength;
      if (totalDownloaded + thisPieceLength > totalLength) {
          thisPieceLength = totalLength - totalDownloaded;
      }
      currentPieceBuffer_.resize(thisPieceLength);
    }

    // If we have requested all blocks for this piece
    if (nextBlockOffset_ >= currentPieceBuffer_.size()) {
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
    if (nextBlockOffset_ + BLOCK_SIZE > thisPieceLength) {
        blockLength = thisPieceLength - nextBlockOffset_;
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

void Peer::setAmInterested(bool interested) {
  if (interested && !amInterested_) {
    log("Session says we are interested. Sending INTERESTED.");
    sendInterested();
    amInterested_ = true;
  } else if (!interested && amInterested_) {
    amInterested_ = false;
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
      log("Received unhandled message. ID: " + std::to_string((int)msg.id));
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
  log("Received CHOKE");
  peerChoking_ = true;

  // If we had requests pending, they are now dead.
  // A real client might re-queue these.
  // For now, we just clear them.
  if (!inFlightRequests_.empty()) {
    log("Peer choked us, clearing requests.");
    inFlightRequests_.clear();
    
    // We must reset our download position to the start
    // of the piece we were working on.
    uint32_t chokedPieceIndex = inFlightRequests_[0].pieceIndex;
    nextBlockOffset_ = 0; // Reset to 0 so we must re-find a piece
    inFlightRequests_.clear();

    if (auto session = session_.lock()) {
      log("Choked, un-assigning piece" + std::to_string(chokedPieceIndex));
      session->unassignPiece(chokedPieceIndex);
    }
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
  log("Received UNCHOKE");
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
    logError("Invalid HAVE message payload size: "+ std::to_string(msg.payload.size()));
    return;
  }
  
  uint32_t pieceIndex;
  memcpy(&pieceIndex, msg.payload.data(), 4);
  pieceIndex = ntohl(pieceIndex);

  log("Received HAVE for piece " + std::to_string(pieceIndex));
  
  // Update local bitfield state
  setHavePiece(pieceIndex);

  // Report to session
  if (auto session = session_.lock()) {
    session->onHaveReceived(shared_from_this(), pieceIndex);
  }
}

/**
 * @brief Handles a have bitfield from a peer
 * 
 * Updates bitfield of peer
 * 
 * A have message means this peer has this signified piece
 */
void Peer::handleBitfield(const PeerMessage& msg) {
  log("Received BITFIELD (" + std::to_string(msg.payload.size()) + " bytes)");

  // Update local bitfield state
  bitfield_ = msg.payload;

  // Report to session
  if (auto session = session_.lock()) {
    session->onBitfieldReceived(shared_from_this(), msg.payload);
  }
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
      logError("Invalid PIECE message payload size: " + std::to_string(msg.payload.size()));
      return;
    }

    // Copy data and convert to host byte order
    uint32_t pieceIndex, begin;
    memcpy(&pieceIndex, msg.payload.data(), 4);
    memcpy(&begin, msg.payload.data() + 4, 4);
    pieceIndex = ntohl(pieceIndex);
    begin = ntohl(begin);
    size_t blockLength = msg.payload.size() - 8;

    log("Received PIECE: Index=" + std::to_string(pieceIndex));
    log(", Begin=" + std::to_string(begin));
    log(", Length=" + std::to_string(blockLength));

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
        log("  Saved " + std::to_string(blockLength) + " bytes to piece buffer.");
      } else {

        log("  WARNING: Received piece data for wrong piece/offset. Discarding.");
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
        log("COMPLETED PIECE " + std::to_string(pieceIndex) + " (all blocks received)!");

        auto session = session_.lock();
        if (!session) return; // Session is gone


        // Verify hash
        if (verifyPieceHash(pieceIndex, session)) {
          log("HASH OK for piece" + std::to_string(pieceIndex));
          
          // Set client bitfield
          session->updateMyBitfield(pieceIndex);
          
          // Call write callback
          session->onPieceCompleted(pieceIndex, currentPieceBuffer_);
          
          // Advance to next piece
          nextBlockOffset_ = 0;
          nextPieceIndex_++;
          currentPieceBuffer_.clear();

        } else {
          logError("*** HASH FAILED *** for piece " + std::to_string(pieceIndex));
          // Discard data and reset to re-download this piece
          nextBlockOffset_ = 0;
          // nextPieceIndex_ remains the same
          currentPieceBuffer_.clear();

          // Unassign piece
          session->unassignPiece(pieceIndex);
        }
      }

    } else {
      log("  WARNING: Received a PIECE that doesn't match any request.");
    }
}


// --- Helper Functions ---

/**
 * @brief Checks if *we* have a specific piece.
 */
bool Peer::clientHasPiece(size_t pieceIndex) const {
  if (auto session = session_.lock()) {
    return session->clientHasPiece(pieceIndex);
  }
  return false;
}

/**
 * @brief Verifies the SHA-1 hash of the piece in currentPieceBuffer_.
 */
bool Peer::verifyPieceHash(size_t pieceIndex, std::shared_ptr<TorrentSession> session) {
  if (!session) {
    logError("ERROR: Cannot verify hash, no session.");
    return false;
  }
  if (currentPieceBuffer_.empty()) {
    logError("ERROR: Cannot verify hash, piece buffer is empty.");
    return false;
  }
  
  unsigned char calculatedHash[SHA_DIGEST_LENGTH]; // 20 bytes
  SHA1(currentPieceBuffer_.data(), currentPieceBuffer_.size(), calculatedHash);
  
  // --- MODIFIED: Get hash from session ---
  const char* expectedHash = session->getPieceHash(pieceIndex);
  if (!expectedHash) {
    return false; // Invalid index
  }
  
  return memcmp(calculatedHash, expectedHash, SHA_DIGEST_LENGTH) == 0;
}