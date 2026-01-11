#include "peer.h"
#include "ITorrentSession.h" 
#include "IPieceManager.h" 
#include <iostream>
#include <stdexcept>
#include <cstring> // For memcpy
#include <openssl/sha.h>
#include <spdlog/spdlog.h>

// CONSTANTS

// Standard block size
static constexpr uint32_t BLOCK_SIZE = 16384; // 2^14 16KB

/**
 * @brief Constructor for a peer
 */
Peer::Peer(
  std::shared_ptr<PeerConnection> conn, 
  std::string ip, 
  std::shared_ptr<IPieceManager> pieceManager
) : conn_(std::move(conn)), 
    ip_(std::move(ip)), 
    pieceManager_(std::move(pieceManager))
{
  if (!pieceManager_) {
    throw std::runtime_error("Peer initialized without PieceManager");
  }
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
  std::weak_ptr<ITorrentSession> session
) {
  // Store the session context
  session_ = session;
  
  // Create 'this' binding for callbacks
  std::weak_ptr<Peer> self = shared_from_this();
  
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
  std::weak_ptr<ITorrentSession> session
) {
  // Store the session context
  session_ = session;

  std::weak_ptr<Peer> self = shared_from_this();
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
    spdlog::error("[{}] Logic: Handshake failed.", ip_);
    // Connection is already closed by PeerConnection. We just stop.
    return;
  }

  spdlog::info("[{}] Logic: Handshake complete. Sending bitfield.", ip_);
  // @TODO: Store their peerId maybe for console logging
  
  // Now that handshake is done, send our bitfield.
  sendBitfield();
}

/**
 * @brief main logic controller for peer
 * 
 * Updates the state after recieving a message via handleMessage
 * then does an actions based on the new state.
 */
void Peer::onMessageReceived(const boost::system::error_code& ec, std::optional<PeerMessage> msg) {
  if (ec) {
    spdlog::error("[{}] Logic: Disconnected ({})", ip_, ec.message());

    // If we were working on a piece, release the lock
    if (auto session = session_.lock()) {
      if (nextBlockOffset_ > 0 && nextBlockOffset_ < currentPieceBuffer_.size()) {
        spdlog::info("[{}] Disconnected, un-assigning piece {}", ip_, nextPieceIndex_);
        session->unassignPiece(nextPieceIndex_);
      }
    }

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
    spdlog::info("[{}] Sent bitfield ({} bytes)", ip_, payload.size());
  }
}

void Peer::sendInterested() {
  conn_->sendMessage(2, {}); // ID 2 = interested, no payload
}

void Peer::sendRequest(uint32_t pieceIndex, uint32_t begin, uint32_t length) {
  spdlog::info("[{}] Sending REQUEST for piece {} (begin: {}, length: {})", ip_, pieceIndex, begin, length);
            
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

// --- requestPiece ---

/**
 * @brief Helper for requestPiece
 * 
 * Gets a new piece from session and resizes the buffer accordingly
 * 
 * @param session The session object
 */
bool Peer::assignNewPiece(std::shared_ptr<ITorrentSession> session) {
  // Get assigned piece from session
  std::optional<size_t> assignedPiece = session->assignWorkForPeer(shared_from_this());

  if (!assignedPiece) {
    // Session tell us there's no work to be done
    return false; // Stop trying to request
  }

  // We have a piece

  nextPieceIndex_ = *assignedPiece;
  spdlog::info("[{}] Session assigned us piece: {}", ip_, nextPieceIndex_);

  long long pieceLength = session->getPieceLength();
  long long totalLength = session->getTotalLength();

  long long thisPieceLength = pieceLength;

  long long totalDownloaded = (long long)nextPieceIndex_ * pieceLength;

  // Verify piece is inbounds
  if (totalDownloaded >= totalLength) {
    spdlog::error("[{}] Critical: Assigned piece index {} starts at {} which is >= total length {}", 
                 ip_, nextPieceIndex_, totalDownloaded, totalLength);
    throw std::runtime_error("Assigned piece out of bounds");
  }

  if (totalDownloaded + thisPieceLength > totalLength) {
      thisPieceLength = totalLength - totalDownloaded;
  }
  currentPieceBuffer_.resize(thisPieceLength);

  return true;
}

/**
 * @brief Helper for requestPiece
 * 
 * Sends a request for the next block in the piece
 * 
 * @param pieceLength The length of the current assigned piece
 */
void Peer::requestNextBlock(long long pieceLength) {
  // Client have a piece to download
  uint32_t blockLength = BLOCK_SIZE;
  
  // TODO: Need to handle the last piece/block, which might be shorter.
  if (nextBlockOffset_ + BLOCK_SIZE > pieceLength) {
      blockLength = pieceLength - nextBlockOffset_;
  }
  
  // Send the request
  spdlog::info("[{}] --- ACTION: Requesting piece {}, Block offset {} ---", ip_, nextPieceIndex_, nextBlockOffset_);
            
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

/**
 * @brief Fills the request pipeline for the current piece
 * 
 * If there is no current piece asks session to assign one
 *
 * It finds the first piece the peer has that we don't,
 * and then requests all blocks for that piece.
 * 
 * Filling up the request pipeline
 * 
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
    // must find a new piece
    if (nextBlockOffset_ == 0) {
      if (!assignNewPiece(session)) {
        setAmInterested(false); // Session gave no piece no longer interesting
        return;
      }
    }

    // We have requested all blocks for this piece
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
    
    requestNextBlock(thisPieceLength);
  
  }
}

void Peer::setAmInterested(bool interested) {
  if (interested && !amInterested_) {
    spdlog::info("[{}] Session says we are interested. Sending INTERESTED.", ip_);
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
      spdlog::warn("[{}] Received unhandled message. ID: {}", ip_, (int)msg.id);
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
  spdlog::info("[{}] Received CHOKE", ip_);
  peerChoking_ = true;

  // Clear the requests and reset block offset
  if (!inFlightRequests_.empty()) {
    spdlog::info("[{}] Peer choked us, clearing requests.", ip_);

    int inFlightRequestCount = inFlightRequests_.size();
    inFlightRequests_.clear();
    
    // Reset position to last recieved piece
    nextBlockOffset_ = nextBlockOffset_ - (inFlightRequestCount * BLOCK_SIZE);

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
  spdlog::info("[{}] Received UNCHOKE", ip_);
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
    spdlog::error("[{}] Invalid HAVE message payload size: {}", ip_, msg.payload.size());
    return;
  }
  
  uint32_t pieceIndex;
  memcpy(&pieceIndex, msg.payload.data(), 4);
  pieceIndex = ntohl(pieceIndex);

  spdlog::info("[{}] Received HAVE for piece {}", ip_, pieceIndex);
  
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
  spdlog::info("[{}] Received BITFIELD ({} bytes)", ip_, msg.payload.size());

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

// --- handlePiece ---

bool Peer::saveBlockToBuffer(uint32_t pieceIndex, uint32_t begin, const std::vector<unsigned char>& payload) {
  size_t blockLength = payload.size() - 8;
  
  if (pieceIndex == nextPieceIndex_ && (begin + blockLength) <= currentPieceBuffer_.size()) {
    const unsigned char* blockData = payload.data() + 8;
    memcpy(&currentPieceBuffer_[begin], blockData, blockLength);
    spdlog::info("[{}] Saved {} bytes to piece buffer.", ip_, blockLength);
    return true;
  } else {
    spdlog::warn("[{}]    WARNING: Received piece data for wrong piece/offset. Discarding.", ip_);
    return false;
  }
}

void Peer::completePiece(uint32_t pieceIndex) {
  spdlog::info("[{}] COMPLETED PIECE {} (all blocks received)!", ip_, pieceIndex);

  auto session = session_.lock();
  if (!session) return; 

  // Verify hash
  if (verifyPieceHash(pieceIndex, session)) {
    spdlog::info("[{}] HASH OK for piece {}", ip_, pieceIndex);
    
    // Set client bitfield
    session->updateMyBitfield(pieceIndex);
    
    // Call write callback
    session->onPieceCompleted(pieceIndex, currentPieceBuffer_);
    
    // Advance to next piece (Reset state)
    nextBlockOffset_ = 0;
    nextPieceIndex_++;
    currentPieceBuffer_.clear();


  } else {
    failedHashCount_++;
    spdlog::error("[{}] *** HASH FAILED *** for piece {} (Strike {}/{})", ip_, pieceIndex, failedHashCount_, (int)MAX_BAD_HASHES);

  if (failedHashCount_ >= MAX_BAD_HASHES) {
    // Close the connection with a protocol error.
    // This will trigger onMessageReceived with the error, which cleans up the session state.
    conn_->close(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
    spdlog::error("[{}] Too many hash failures. Disconnecting peer.", ip_);
    return; 
  }
  // Discard data and reset to re-download this piece
  nextBlockOffset_ = 0;
  currentPieceBuffer_.clear();

  // Unassign piece so others can grab it (or we grab it again later)
  session->unassignPiece(pieceIndex);
  }
}

void Peer::handlePiece(const PeerMessage& msg) {
  if (msg.payload.size() < 8) {
    spdlog::error("[{}] Invalid PIECE message payload size: {}", ip_, msg.payload.size());
    return;
  }

  // Copy data and convert to host byte order
  uint32_t pieceIndex, begin;
  memcpy(&pieceIndex, msg.payload.data(), 4);
  memcpy(&begin, msg.payload.data() + 4, 4);
  pieceIndex = ntohl(pieceIndex);
  begin = ntohl(begin);
  size_t blockLength = msg.payload.size() - 8;

  spdlog::info("[{}] Received PIECE: Index={}", ip_, pieceIndex);
  spdlog::info("[{}] Begin={}", ip_, begin);
  spdlog::info("[{}] Length={}", ip_, blockLength);

  // Find this block from our in-flight list
  auto it = std::find_if(inFlightRequests_.begin(), inFlightRequests_.end(), 
    [pieceIndex, begin, blockLength](const PendingRequest& req) {
        return req.pieceIndex == pieceIndex && 
                req.begin == begin && 
                req.length == blockLength;
    });

  if (it == inFlightRequests_.end()) {
    spdlog::error("[{}]   ERROR: Received a PIECE that doesn't match any request.", ip_);
    return;
  }

  // Remove from buffer
  inFlightRequests_.erase(it);

  if (!saveBlockToBuffer(pieceIndex, begin, msg.payload)) {
    return; // Failed
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
    completePiece(pieceIndex);
  }

}


// --- Helper Functions ---

/**
 * @brief Checks if the client has a specific piece.
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
bool Peer::verifyPieceHash(size_t pieceIndex, std::shared_ptr<ITorrentSession> session) {
  if (!session) {
    spdlog::error("[{}] ERROR: Cannot verify hash, no session.", ip_);
    return false;
  }
  if (currentPieceBuffer_.empty()) {
    spdlog::error("[{}] ERROR: Cannot verify hash, piece buffer is empty.", ip_);
    return false;
  }
  
  unsigned char calculatedHash[SHA_DIGEST_LENGTH]; // 20 bytes
  SHA1(currentPieceBuffer_.data(), currentPieceBuffer_.size(), calculatedHash);
  
  // --- Get hash from session ---
  const char* expectedHash = session->getPieceHash(pieceIndex);
  if (!expectedHash) {
    return false; // Invalid index
  }
  
  return memcmp(calculatedHash, expectedHash, SHA_DIGEST_LENGTH) == 0;
}