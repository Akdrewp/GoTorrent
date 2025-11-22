#include "torrentSession.h"
#include "bencode.h" // For BencodeValue, printBencodeValue
#include <iostream>
#include <iomanip>    // For std::setw, std::hex
#include <variant>    // For std::visit, std::get_if
#include <stdexcept>  // For std::runtime_error
#include <functional> // For std::bind, std::placeholders

//--- Bencode Printer ---

static void printBencodeValue(const BencodeValue& bv, int indent = 0);
struct BencodePrinter {
  int indent;
  void operator()(long long val) const { std::cout << val; }
  void operator()(const std::string& val) const {
    if (indent > 2) { std::cout << "\"(... compact peers data ...)\""; return; }
    std::cout << "\"";
    for(unsigned char c : val) {
      if (std::isprint(c) && c != '\\' && c != '\"') { std::cout << c; }
      else { std::cout << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::dec; }
    }
    std::cout << "\"";
  }
  void operator()(const BencodeList& list) const {
    std::cout << "[\n";
    for (const auto& item_ptr : list) {
      std::cout << std::string(indent + 2, ' ');
      printBencodeValue(*item_ptr, indent + 2);
      std::cout << ",\n";
    }
    std::cout << std::string(indent, ' ') << "]";
  }
  void operator()(const BencodeDict& dict) const {
    std::cout << "{\n";
    for (const auto& [key, val_ptr] : dict) {
      std::cout << std::string(indent + 2, ' ') << "\"" << key << "\": ";
      if (key == "pieces") { std::cout << "\"(... pieces data redacted ...)\""; }
      else { printBencodeValue(*val_ptr, indent + 2); }
      std::cout << ",\n";
    }
    std::cout << std::string(indent, ' ') << "}";
  }
};
static void printBencodeValue(const BencodeValue& bv, int indent) {
  std::visit(BencodePrinter{indent}, bv.value);
}

// --- TorrentSession class ---

TorrentSession::TorrentSession(
  asio::io_context& io_context, 
  TorrentData torrent,
  std::string& peerId,
  unsigned short port
) : io_context_(io_context),
    peerId_(peerId),
    port_(port),
    torrent_(std::move(torrent))
{
}

/**
 * @brief Starts the session
 * 
 * Loads torrent info, contacts and connects to peer
 */
void TorrentSession::start() {
  std::cout << "--- Starting Torrent Session ---" << std::endl;
  loadTorrentInfo();
  requestPeers();
  connectToPeers();
}

void TorrentSession::loadTorrentInfo() {
  // Calculate our bitfield size
  auto& infoVal = torrent_.mainData.at("info")->value;
  auto* infoDict = std::get_if<BencodeDict>(&infoVal);
  if (!infoDict) throw std::runtime_error("Invalid info dictionary.");

  auto& piecesVal = infoDict->at("pieces")->value;
  auto* piecesStr = std::get_if<std::string>(&piecesVal);
  if (!piecesStr) throw std::runtime_error("Invalid 'pieces' string.");

  // Store the pieces hashes
  pieceHashes_ = *piecesStr;

  // Each piece has a hash in "pieces" each with length 20
  // Amount of pieces is length of "pieces" / 20
  numPieces_ = piecesStr->length() / 20;

  auto& pieceLenVal = infoDict->at("piece length")->value;
  if (auto* len = std::get_if<long long>(&pieceLenVal)) {
    pieceLength_ = *len;
  } else {
    throw std::runtime_error("Invalid 'piece length'.");
  }
  totalLength_ = getTotalLengthTorrent(*infoDict); 
  std::cout << "Torrent piece length: " << pieceLength_ << std::endl;
  std::cout << "Torrent total length: " << totalLength_ << std::endl;

  // Get file name for output (Single file)
  if (infoDict->count("name")) {
    if (auto* name = std::get_if<std::string>(&(infoDict->at("name")->value))) {
      outputFilename_ = *name;
      std::cout << "Output file: " << outputFilename_ << std::endl;

      // Open the file for binary read/write.
      // Create it if it doesn't exist.
      outputFile_.open(outputFilename_, std::ios::binary | std::ios::in | std::ios::out);
      if (!outputFile_.is_open()) {
        std::cout << "File not found, creating..." << std::endl;
        outputFile_.open(outputFilename_, std::ios::binary | std::ios::out);
      }
      
      if (!outputFile_.is_open()) {
        throw std::runtime_error("Failed to open or create output file: " + outputFilename_);
      }
    } else {
      throw std::runtime_error("Torrent 'name' is not a string.");
    }
  } else {
    throw std::runtime_error("Torrent has no 'name' (multi-file torrents not supported).");
  }

  // Each bit in the bitfield corresponds to a piece being recieved
  //
  // Need amount of pieces / 8 (bits in byte) rounded up to hold
  // trailing bits
  size_t bitfieldSize = (numPieces_ + 7) / 8; // ceil(numPieces / 8.0)

  // Create our bitfield (all zeros, we have no pieces)
  myBitfield_.resize(bitfieldSize, 0);

  // Initialize rarity map
  pieceAvailability_.resize(numPieces_, 0);

  // Print the hash
  std::cout << "--- INFO HASH ---" << std::endl;
  std::cout << std::hex << std::setfill('0');
  for (unsigned char c : torrent_.infoHash) {
    std::cout << std::setw(2) << static_cast<int>(c);
  }
  std::cout << std::dec << std::endl;
}

void TorrentSession::requestPeers() {
  // Get announce URL
  auto& announceVal = torrent_.mainData.at("announce")->value;
  std::string announceUrl;
  if (auto* url = std::get_if<std::string>(&announceVal)) {
    announceUrl = *url;
  } else {
    throw std::runtime_error("Announce URL is not a string.");
  }

  // Build the Tracker GET URL
  std::string trackerUrl = buildTrackerUrl(
    announceUrl,
    torrent_.infoHash,
    peerId_,
    port_,
    0,    // uploaded
    0,    // downloaded
    totalLength_,
    1     // compact
  );

  // Print request details
  std::cout << "\n--- PREPARING TRACKER REQUEST ---" << std::endl;
  std::cout << "Announce URL: " << announceUrl << std::endl;
  std::cout << "Total Length (left): " << totalLength_ << std::endl;

  std::cout << "\n--- SENDING REQUEST TO TRACKER ---" << std::endl;
  std::string trackerResponse = sendTrackerRequest(trackerUrl);
  std::cout << "Tracker raw response size: " << trackerResponse.size() << " bytes" << std::endl;

  // Parse the tracker's response
  std::cout << "\n--- PARSING TRACKER RESPONSE ---" << std::endl;
  size_t index = 0;
  std::vector<char> responseBytes(trackerResponse.begin(), trackerResponse.end());
  BencodeValue parsedResponse = parseBencodedValue(responseBytes, index);
  
  printBencodeValue(parsedResponse);
  std::cout << std::endl;

  // Get peer list
  std::cout << "\n--- PARSED PEER LIST ---" << std::endl;
  if (auto* respDict = std::get_if<BencodeDict>(&parsedResponse.value)) {
    if (respDict->count("failure reason")) {
      auto& failVal = respDict->at("failure reason")->value;
      if (auto* failStr = std::get_if<std::string>(&failVal)) {
        throw std::runtime_error("Tracker error: " + *failStr);
      }
    }

    if (respDict->count("peers")) {
      auto& peersVal = respDict->at("peers")->value;
      if (auto* peersStr = std::get_if<std::string>(&peersVal)) {
        trackerPeers_ = parseCompactPeers(*peersStr); // Store peers in member
        for (const auto& peer : trackerPeers_) {
          std::cout << "Peer: " << peer.ip << ":" << peer.port << std::endl;
        }
      } else {
        std::cout << "(Peers key was not a compact string)" << std::endl;
      }
    }
  } else {
    throw std::runtime_error("Tracker response was not a dictionary.");
  }
}

void TorrentSession::connectToPeers() {
  if (trackerPeers_.empty()) {
    std::cout << "\nNo peers found from tracker." << std::endl;
    return;
  }

  std::cout << "\n--- CONNECTING TO PEERS ---" << std::endl;
  using namespace std::placeholders; // for _1, _2

  // Try to connect to every peer
  for (const auto& peerInfo : trackerPeers_) {
    // Create the connection object
    auto peer = std::make_shared<Peer>(io_context_, peerInfo.ip, peerInfo.port);
    std::cout << "Attempting async connect to " << peerInfo.ip << ":" << peerInfo.port << std::endl;

    // This sends bitfield, waits for reply and completes handshake
    peer->startAsOutbound(
      torrent_.infoHash,
      peerId_,
      shared_from_this() // Pass self as session
    );

    // Add peer to peer lists
    activePeers_.push_back(peer);
  }
  
  std::cout << "--- CONNECTED TO " << activePeers_.size() << " PEERS ---" << std::endl;
}

void TorrentSession::handleInboundConnection(tcp::socket socket) {
  std::cout << "\n--- INBOUND CONNECTION from " 
            << socket.remote_endpoint().address().to_string() 
            << " ---" << std::endl;
  
  // Create a PeerConnection object from the existing socket
  auto peer = std::make_shared<Peer>(io_context_, std::move(socket));
  using namespace std::placeholders; // for _1, _2

  // Start the inbound connection process
  // This will wait for the peer's handshake, then reply
  peer->startAsOutbound(
    torrent_.infoHash,
    peerId_,
    shared_from_this() // Pass self as session
  );
  // Add peer to peer lists
  activePeers_.push_back(peer);
}

void TorrentSession::onPieceCompleted(size_t pieceIndex, std::vector<uint8_t> data) {
  unassignPiece(pieceIndex);

  if (!outputFile_.is_open()) {
    std::cerr << "--- SESSION: ERROR! Output file is not open. Cannot write piece " << pieceIndex << " ---" << std::endl;
    return;
  }

  // Calculate the byte offset in the file
  long long offset = static_cast<long long>(pieceIndex) * pieceLength_;
  std::cout << "--- SESSION: Writing piece " << pieceIndex << " to disk at offset " << offset << " (" << data.size() << " bytes) ---" << std::endl;

  // Seek to the correct position
  outputFile_.seekp(offset, std::ios::beg);
  if (outputFile_.fail()) {
      std::cerr << "--- SESSION: ERROR! Failed to seek to offset " << offset << " ---" << std::endl;
      outputFile_.clear(); // Clear error state
      return;
  }

  // Write the data at position
  outputFile_.write(reinterpret_cast<const char*>(data.data()), data.size());
  if (outputFile_.fail()) {
      std::cerr << "--- SESSION: ERROR! Failed to write data for piece " << pieceIndex << " ---" << std::endl;
      outputFile_.clear(); // Clear error state
      return;
  }

  // Flush the buffer to ensure it's written to disk
  outputFile_.flush();
  if (outputFile_.fail()) {
      std::cerr << "--- SESSION: ERROR! Failed to flush file stream for piece " << pieceIndex << " ---" << std::endl;
      outputFile_.clear(); // Clear error state
  }
}

/**
 * @brief When a peer sends their bitfield, update rarity map
 */
void TorrentSession::onBitfieldReceived(std::shared_ptr<Peer> peer, std::vector<uint8_t> bitfield) {
  // Update the global piece rarity map
  for (size_t i = 0; i < numPieces_; ++i) {
    size_t byte_index = i / 8;
    uint8_t bit_index = 7 - (i % 8);
    if (byte_index < bitfield.size() && (bitfield[byte_index] & (1 << bit_index))) {
      pieceAvailability_[i]++;
    }
  }
  
  // Check if this peer is now interesting
  checkIfPeerIsInteresting(peer);
}

void TorrentSession::checkIfPeerIsInteresting(std::shared_ptr<Peer> peer) {
  // Check if we're already interested
  if (peer->amInterested_) {
    return;
  }
  
  // Check if they have any piece we don't
  for (size_t i = 0; i < numPieces_; ++i) {
    if (peer->hasPiece(i)) {
      if (!clientHasPiece(i)) {
        // They have a piece we need
        peer->setAmInterested(true);
        return;
      }
    }
  }
}

/**
 * @brief (SESSION) A peer reported it now has a new piece.
 */
void TorrentSession::onHaveReceived(std::shared_ptr<Peer> peer, size_t pieceIndex) {
  if (pieceIndex >= numPieces_) return; // Invalid index
  
  // Update global rarity
  pieceAvailability_[pieceIndex]++;

  // Check if this peer is now interesting
  checkIfPeerIsInteresting(peer);
}

/**
 * @brief Finds the best piece for the peer to start requesting
 */
std::optional<size_t> TorrentSession::assignWorkForPeer(std::shared_ptr<Peer> peer) {
  // TODO: This is "first available" strategy.
  // Need to implement rarest first

  for (size_t i = 0; i < numPieces_; ++i) {
    // Check if peer has this piece
    if (peer->hasPiece(i)) {
      
      // Check if client doesn't have this piece
      if (!clientHasPiece(i)) {

        // Check it's not already assigned
        if (assignedPieces_.count(i) == 0) {
          std::cout << "--- SESSION: Assigning piece " << i << " ---" << std::endl;
          assignedPieces_.insert(i);
          return i;
        }
      }
    }
  }

  // No pieces for this peer
  return std::nullopt;
}

/**
 * @brief Unlocks a piece when a peer is done
 */
void TorrentSession::unassignPiece(size_t pieceIndex) {
  if (assignedPieces_.count(pieceIndex)) {
    std::cout << "--- SESSION: Un-assigning piece " << pieceIndex << " ---" << std::endl;
    assignedPieces_.erase(pieceIndex);
  }
}

/**
 * @brief Checks whether client has this piece
 */
bool TorrentSession::clientHasPiece(size_t pieceIndex) const {
  size_t byte_index = pieceIndex / 8;
  uint8_t bit_index = 7 - (pieceIndex % 8);
  if (byte_index >= myBitfield_.size()) {
    return false;
  }
  return (myBitfield_[byte_index] & (1 << bit_index)) != 0;
}

/**
 * @brief Gets the piece hash for a piece
 */
const char* TorrentSession::getPieceHash(size_t pieceIndex) const {
  if (pieceIndex * 20 >= pieceHashes_.size()) {
    return nullptr;
  }
  return pieceHashes_.data() + (pieceIndex * 20);
}

/**
 * @brief Updates clients bitfield
 */
void TorrentSession::updateMyBitfield(size_t pieceIndex) {
  size_t my_byte_index = pieceIndex / 8;
  uint8_t my_bit_index = 7 - (pieceIndex % 8);
  if (my_byte_index < myBitfield_.size()) {
    myBitfield_[my_byte_index] |= (1 << my_bit_index);
  }
}