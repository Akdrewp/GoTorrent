#include "torrentSession.h"
#include "bencode.h" // For BencodeValue, printBencodeValue
#include <iostream>
#include <iomanip>    // For std::setw, std::hex
#include <variant>    // For std::visit, std::get_if
#include <stdexcept>  // For std::runtime_error
#include <functional> // For std::bind, std::placeholders
#include <random> // For random

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
  unsigned short port,
  std::shared_ptr<ITrackerClient> trackerClient,
  std::shared_ptr<ITorrentStorage> storage
) : io_context_(io_context),
    peerId_(peerId),
    port_(port),
    torrent_(std::move(torrent)),
    trackerClient_(std::move(trackerClient)),
    storage_(std::move(storage))
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

// --- loadTorrentInfo ---

/**
 * Helper function for loadTorrentInfo
 * 
 * @brief Extracts the piece length from the info dictionary.
 * 
 * @param infoDict The BencodeDict from .torrent file
 */
static long long getTorrentPieceLength(const BencodeDict& infoDict) {
  if (infoDict.count("piece length")) {
    auto& lengthVal = infoDict.at("piece length")->value;
    if (auto* len = std::get_if<long long>(&lengthVal)) {
      std::cout << "Torrent piece length: " << *len << std::endl;
      return *len;
    }
  }
  throw std::runtime_error("Invalid 'piece length'.");
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

  pieceLength_ = getTorrentPieceLength(*infoDict);
  totalLength_ = getTotalLengthTorrent(*infoDict);

  // TODO CHANGE
  std::string DOWNLOAD_PATH = "."; // Current directory
  // Initialize file storage
  storage_->initialize(torrent_, pieceLength_, DOWNLOAD_PATH);

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
  std::string trackerResponse = trackerClient_.get()->sendRequest(trackerUrl);
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

// --- connectToPeers ---

static std::shared_ptr<Peer> initPeer(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port) {
  // Create Connection (Transport Layer)
  auto conn = std::make_shared<PeerConnection>(io_context, peer_ip, peer_port);
  
  // Inject Connection into Peer (Logic Layer)
  auto peer = std::make_shared<Peer>(conn, peer_ip);

  return peer;
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

    // Create peer
    auto peer = initPeer(io_context_, peerInfo.ip, peerInfo.port);

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
  
  // Create Connection
  auto conn = std::make_shared<PeerConnection>(io_context_, std::move(socket));
  
  // Create peer
  auto peer = std::make_shared<Peer>(conn, conn->get_ip());

  // Start the inbound connection process
  // This waits for the peer's handshake, then reply
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

  storage_->writePiece(pieceIndex, data);

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

// --- assignWorkForPeer ---

/**
 * @brief Gets the pieces the peer has sorted from least to most rare
 */
static std::vector<size_t> getSortedCandidatePieces(
  const std::shared_ptr<Peer>& peer,
  size_t numPieces,
  const std::vector<size_t>& pieceAvailability,
  const std::set<size_t>& assignedPieces,
  std::function<bool(size_t)> clientHasPieceFn
) {
  struct Candidate {
    size_t index;
    size_t rarity;
  };

  std::vector<Candidate> candidates;
  // Reserve to avoid reallocations
  candidates.reserve(numPieces);

  // Get valid piece that:
  // Peer has AND client doesn't AND not currently assigned
  for (size_t i = 0; i < numPieces; ++i) {
    if (peer->hasPiece(i)) {
      if (!clientHasPieceFn(i)) {
        if (assignedPieces.count(i) == 0) {
          // Valid candidate
          candidates.push_back({ i, pieceAvailability[i] });
        }
      }
    }
  }

  // Sort candidates by Rarity (Ascending), then by Index
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.rarity != b.rarity) {
      return a.rarity < b.rarity; // Prioritize rarer pieces
    }
    // Same rarity choose lower index
    return a.index < b.index;
  });

  // Extract sorted indices
  std::vector<size_t> sortedIndices;
  sortedIndices.reserve(candidates.size());
  for (const auto& c : candidates) {
    sortedIndices.push_back(c.index);
  }

  return sortedIndices;
}

/**
 * @brief Gets top randomVariance pieces from pieces array and returns one at random
 * 
 * @param pieces The pieces array sorted from most to least rare
 * @param randomVariance How many pieces to choose from the top
 * 
 * @returns The index of randomly chosen piece
 */
static size_t getTopRandomPiece(std::vector<size_t> pieces, int randomVariance) {
  size_t poolSize = std::min(pieces.size(), static_cast<size_t>(randomVariance));
  
  static std::random_device rd;
  static std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, poolSize - 1);

  size_t chosenIndex = pieces[dis(gen)];

  std::cout << "--- SESSION: Assigning piece " << chosenIndex;
  
  return chosenIndex;
}

/**
 * @brief Finds the best piece for the peer to start requesting
 * 
 * Uses the Rarest first strategy:
 * Selects the piece that the least amount of peers have
 * 
 * @param peer The peer to assign a piece to
 */
std::optional<size_t> TorrentSession::assignWorkForPeer(std::shared_ptr<Peer> peer) {
  // Rarest First with Randomness
  // 1. Get peer pieces sorted by rarity
  // 2. Choose from top N rarest pieces where N is variance

  std::vector<size_t> sortedPieces = getSortedCandidatePieces(
    peer, 
    numPieces_, 
    pieceAvailability_, 
    assignedPieces_, 
    [this](size_t i) { return clientHasPiece(i); }
  );

  // If no candidates, we have nothing for this peer to do
  if (sortedPieces.empty()) {
    return std::nullopt;
  }

  int chosenPieceIndex = getTopRandomPiece(sortedPieces, 1); // Random variance = 1 for now

  std::cout << "--- SESSION: Assigning piece " << chosenPieceIndex;

  // Lock the piece so no one else picks it
  assignedPieces_.insert(chosenPieceIndex);
  
  return chosenPieceIndex;
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