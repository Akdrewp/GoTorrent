#include "peer.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <algorithm>
#include <arpa/inet.h>
#include "ITorrentSession.h"

// CONSTANTS

// Standard block size
static constexpr uint32_t BLOCK_SIZE = 16384; // 2^14 16KB

Peer::Peer(
    std::shared_ptr<PeerConnection> conn, 
    std::string ip, 
    std::shared_ptr<IPieceRepository> repo,
    std::shared_ptr<IPiecePicker> picker
) : conn_(std::move(conn)), 
    ip_(std::move(ip)), 
    repo_(std::move(repo)), 
    picker_(std::move(picker)) 
{
    if (!repo_ || !picker_) throw std::runtime_error("Peer dependencies cannot be null");
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
    if (!bitfield_.empty()) {
      picker_->processPeerDisconnect(bitfield_);
    }

    if (nextBlockOffset_ > 0 || !inFlightRequests_.empty()) {
      spdlog::info("[{}] Disconnected, un-assigning piece {}", ip_, nextPieceIndex_);
      picker_->onPieceFailed(nextPieceIndex_);
    }

    // Notify session
    if (auto session = session_.lock()) {
      session->onPeerDisconnected(shared_from_this());
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
    std::vector<unsigned char> payload = repo_->getBitfield();
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
 * 
 * 1. Check if peer has pieces client doesn't 
 * by comparing bitfields then setAmInterested to true
 * 
 * 2. Call requestPiece if peer is interesting and not choking
 */
void Peer::doAction() {
  // 1. Check if peer is interesting
  if(!amInterested_) {
    std::vector<uint8_t> myBitfield = repo_->getBitfield();

    bool interesting = false;
    // Compare bitfields byte by byte
    size_t limit = std::min(bitfield_.size(), myBitfield.size());
    for (size_t i = 0; i < limit; ++i) {
      if ((bitfield_[i] & ~myBitfield[i]) != 0) {
        interesting = true;
        break;
      }
    }
    
    // Check trailing bits if peer has more pieces than us
    if (!interesting && bitfield_.size() > limit) {
      for (size_t i = limit; i < bitfield_.size(); ++i) {
        if (bitfield_[i] != 0) { interesting = true; break; }
      }
    }

    if (interesting) {
        setAmInterested(true);
    }
  }

  if (amInterested_ && !peerChoking_) {
    requestPiece();
  }
}

// --- requestPiece ---

/**
 * @brief Helper for requestPiece
 * Assigns peer a new piece
 * 
 * 1. Gets client bitfield
 * 2. Pass peer bitfield and clientBitfield to piecePicker
 * 3. Assigns nextPieceIndex to returned piece
 * 4. Sets and resizes buffers according to which piece
 */
bool Peer::assignNewPiece() {
  // 1. Get our current inventory
  auto clientBitfield = repo_->getBitfield();

  // 2. Ask the Picker for a job
  auto assignment = picker_->pickPiece(bitfield_, clientBitfield);

  if (!assignment) return false; // Nothing to do

  // 3. Assign nextPiece
  nextPieceIndex_ = *assignment;
  spdlog::info("[{}] Picker assigned piece: {}", ip_, nextPieceIndex_);

  // 4. Setup and resize buffers
  size_t pieceLen = repo_->getPieceLength();
  size_t totalLen = repo_->getTotalLength();
  
  size_t thisLen = pieceLen;

  // Last piece has length of data left in torrent
  // Ex. Piece length is 10 and total length is 25 then last piece is 5
  if ((nextPieceIndex_ * pieceLen) + thisLen > totalLen) { //If last piece
    thisLen = totalLen - (nextPieceIndex_ * pieceLen);
  }

  currentPieceBuffer_.clear();
  currentPieceBuffer_.resize(thisLen);
  nextBlockOffset_ = 0;
  
  return true;
}

/**
 * @brief Helper for requestPiece
 * 
 * Called when peer has a piece to download
 * 
 * 1. Sends a request for the next block in the piece through network
 * 2. Adds requested piece to buffer
 * 3. Advances next block offset
 * @param pieceLength The length of the current assigned piece
 */
void Peer::requestNextBlock(long long pieceLength) {
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
 * loop until MAX_PIPELINE_SIZE has been reached
 *   1. Gets piece from piecePicker if not currently assigned
 *   2. Check if all blocks have been requested for piece
 *   3. Send block request
 */
void Peer::requestPiece() {

  if (session_.expired()) return;

  // Loop until pipeline is full
  while (inFlightRequests_.size() < MAX_PIPELINE_SIZE) {
    
    // 1. Get piece assignment from piecePicker
    // if peer doesn't have active assignment
    if (nextBlockOffset_ == 0) {
      if (!assignNewPiece()) {
        // No pieces assigned
        setAmInterested(false); 
        return;
      }
    }

    // 2. Check if peer has requested every block for current piece
    if (nextBlockOffset_ >= currentPieceBuffer_.size()) {
      return;
    }

    // Ensure buffer is valid
    long long thisPieceLength = currentPieceBuffer_.size();
    if (thisPieceLength == 0) {
      nextBlockOffset_ = 0;
      return;
    }

    // 3. Request next block of piece
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

    // request: <len=0013><id=6><index><begin><length>
    case 6: handleRequest(msg); break;

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
 * Any existing requests should be considered to be discarded
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
 * Updates bitfield of peer through set have piece.
 * Notifies piecePicker.
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

  // Report to piecePicker
  picker_->processHave(pieceIndex);
}

/**
 * @brief Handles a have bitfield from a peer
 * 
 * Updates bitfield_ of peer.
 * Reports bitfield to piecePicker.
 */
void Peer::handleBitfield(const PeerMessage& msg) {
  spdlog::info("[{}] Received BITFIELD ({} bytes)", ip_, msg.payload.size());

  // Update local bitfield state
  bitfield_ = msg.payload;

  // Report to piecePicker
  picker_->processBitfield(bitfield_);
}

// --- handleRequest ---

/**
 * @brief Static helper for handleRequest
 * Reads data from disk and sends a PIECE message.
 * 1. Reads block from repository
 * 2. Constructs payload <index><begin><block>
 * 3. Sends PIECE message with payload
 */
static void fulfillRequest(
    const std::shared_ptr<IPieceRepository>& repo,
    const std::shared_ptr<PeerConnection>& conn,
    const std::string& ip,
    uint32_t index,
    uint32_t begin,
    uint32_t length
) {
  try {
    std::vector<uint8_t> block = repo->readBlock(index, begin, length);
    
    // 1. Construct PIECE message: <index><begin><block>
    std::vector<uint8_t> payload(8 + block.size());
    
    uint32_t netIndex = htonl(index);
    uint32_t netBegin = htonl(begin);
    
    // Copy headers and data into the payload buffer
    memcpy(payload.data(), &netIndex, 4);
    memcpy(payload.data() + 4, &netBegin, 4);
    memcpy(payload.data() + 8, block.data(), block.size());
    
    conn->sendMessage(7, payload);
    
  } catch (const std::exception& e) {
    spdlog::warn("[{}] Could not fulfill request for piece {} offset {}: {}", ip, index, begin, e.what());
  }
}

/**
 * @brief Handles a Request message from peer
 * 
 * Send the block to a peer who is not being choked
 * 
 * 1. Ensure peer is not being choked
 * 2. Validate header and message payload
 * 3. Send block
 */
void Peer::handleRequest(const PeerMessage& msg) {
  if (amChoking_) {
    // If we are choking the peer, we ignore requests.
    return;
  }

  // 2. Validate message and convert to host byte order
  if (msg.payload.size() != 12) {
    spdlog::warn("[{}] Invalid REQUEST size: {}", ip_, msg.payload.size());
    return;
  }
  uint32_t index, begin, length;
  memcpy(&index, msg.payload.data(), 4);
  memcpy(&begin, msg.payload.data() + 4, 4);
  memcpy(&length, msg.payload.data() + 8, 4);
  index = ntohl(index);
  begin = ntohl(begin);
  length = ntohl(length);

  if (length > 131072) {
    spdlog::warn("[{}] Request too large: {}", ip_, length);
    return;
  }

  // 3. Send block to peer
  fulfillRequest(repo_, conn_, ip_, index, begin, length);
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

/**
 * @brief Static helper to handle piece hash failures
 * Resets state and notifies the picker to re-queue the piece.
 * 
 * Increments failedHashCount_ and checks ifs more than the
 * maximum bad hashes allowed. Disconnects if it has been
 * reached.
 */
static void handleHashFail(
  const std::shared_ptr<IPiecePicker>& picker,
  const std::shared_ptr<PeerConnection>& conn,
  const std::string& ip,
  uint32_t pieceIndex,
  uint32_t& nextBlockOffset, 
  std::vector<uint8_t>& currentPieceBuffer,
  int& failedHashCount,
  const int& MAX_BAD_HASHES
) {
  picker->onPieceFailed(pieceIndex);
  
  // Reset state
  nextBlockOffset = 0;
  currentPieceBuffer.clear();

  failedHashCount++;
  spdlog::error("[{}] Hash/Write FAILED for piece {} (Strike {}/{})", ip, pieceIndex, failedHashCount, MAX_BAD_HASHES);

  // Disconnect if limit reached
  if (failedHashCount >= MAX_BAD_HASHES) {
    spdlog::error("[{}] Too many bad hashes. Disconnecting.", ip);
    conn->close(boost::system::errc::make_error_code(boost::system::errc::protocol_error));
  }
}

/**
 * @brief Finishes piece downloading when 
 * all blocks for a piece have been stored in buffer.
 * 
 * 1. Checks hash
 * 2. Saves to disk
 * 3. Notifies piecePicker
 * 4. Resets for next piece
 * 
 * On hash/write fail
 * Calls piecePicker onPieceFailed
 * and handleHashFail
 */
void Peer::completePiece(uint32_t pieceIndex) {
  spdlog::info("[{}] Finished downloading piece {}", ip_, pieceIndex);

  if (repo_->verifyHash(pieceIndex, currentPieceBuffer_)) {
    spdlog::info("[{}] Hash OK. Saving.", ip_);
    
    try {
      repo_->savePiece(pieceIndex, currentPieceBuffer_);
      
      picker_->onPiecePassed(pieceIndex);
      
      // Reset for next piece
      nextBlockOffset_ = 0;
      currentPieceBuffer_.clear();
        
    } catch (...) {
      spdlog::error("Disk write failed");
      picker_->onPieceFailed(pieceIndex);

      nextBlockOffset_ = 0;
      currentPieceBuffer_.clear();
    }
  } else {
    spdlog::error("[{}] Hash FAILED for piece {}", ip_, pieceIndex);
    
    handleHashFail(picker_, conn_, ip_, pieceIndex, nextBlockOffset_, currentPieceBuffer_, failedHashCount_, MAX_BAD_HASHES);
  }
}

/**
 * @brief Processes an incoming PIECE message containing a block of data
 * 
 * 1. Validates message payload data and headers
 * 2. Verifies block matches an existing inFlightRequest
 * 3. Writes block data into the currentPieceBuffer_
 * 4. Checks if the piece is finished and 
 * calls completePiece() if it is
 */
void Peer::handlePiece(const PeerMessage& msg) {
  // 1. Validate data and convert to host byte order
  if (msg.payload.size() < 8) {
    spdlog::error("[{}] Invalid PIECE message payload size: {}", ip_, msg.payload.size());
    return;
  }
  uint32_t pieceIndex, begin;
  memcpy(&pieceIndex, msg.payload.data(), 4);
  memcpy(&begin, msg.payload.data() + 4, 4);
  pieceIndex = ntohl(pieceIndex);
  begin = ntohl(begin);
  size_t blockLength = msg.payload.size() - 8;
  spdlog::info("[{}] Received PIECE: Index={}", ip_, pieceIndex);
  spdlog::info("[{}] Begin={}", ip_, begin);
  spdlog::info("[{}] Length={}", ip_, blockLength);

  // 2. Verify piece is from a request we sent to this peer
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

  // 3. Remove from requests and save to currenPieceBuffer_
  inFlightRequests_.erase(it);
  if (!saveBlockToBuffer(pieceIndex, begin, msg.payload)) {
    return; // Failed
  }

  // 4. Check if this piece is now complete
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

// --- Choking Algorithm ---

double Peer::getDownloadRate() const {
    // TODO: Implement actual rate calculation logic in PeerConnection or Peer
    // For now, return 0 or random for testing
    return 0.0;
}

double Peer::getUploadRate() const {
    // TODO: Implement actual rate calculation logic
    return 0.0;
}

bool Peer::isAmChoking() const {
    return amChoking_;
}

void Peer::setAmChoking(bool choking) {
    if (amChoking_ == choking) return;

    amChoking_ = choking;
    if (amChoking_) {
      spdlog::info("[{}] Choking Peer.", ip_);
      conn_->sendMessage(0, {}); // ID 0 = Choke
    } else {
      spdlog::info("[{}] Unchoking Peer.", ip_);
      conn_->sendMessage(1, {}); // ID 1 = Unchoke
    }
}

std::string Peer::getIp() const {
    return ip_;
}