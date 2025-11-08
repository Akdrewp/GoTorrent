#include "client.h"
#include "peer.h"     // For PeerConnection
#include "bencode.h"  // For BencodeValue, parseBencodedValue

#include <iostream>    // For std::cout, std::cerr
#include <iomanip>     // For std::setw, std::hex
#include <variant>     // For std::visit, std::get_if
#include <stdexcept>   // For std::runtime_error
#include <random>      // For std::random_device, std::mt19937

// --- Constants ---
static constexpr uint32_t BLOCK_SIZE = 16384;

// --- Static Helper Functions (Internal to this file) ---

// Forward declaration
static void printBencodeValue(const BencodeValue& bv, int indent = 0);

/**
 * @brief Helper struct for using std::visit to print BencodeValue
 */
struct BencodePrinter {
  int indent;
  void operator()(long long val) const {
    std::cout << val;
  }
  void operator()(const std::string& val) const {
    // Redact 'peers' string
    if (indent > 2) { // A bit of a heuristic to find the peers value
       std::cout << "\"(... compact peers data ...)\"";
       return;
    }

    std::cout << "\"";
    for(unsigned char c : val) {
      if (std::isprint(c) && c != '\\' && c != '\"') {
        std::cout << c;
      } else {
        std::cout << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::dec;
      }
    }
    std::cout << "\"";
  }
  void operator()(const BencodeList& list) const {
    std::cout << "[\n";
    for (const auto& item_ptr : list) {
      std::cout << std::string(indent + 2, ' ');
      printBencodeValue(*item_ptr, indent + 2); // Recursive call
      std::cout << ",\n";
    }
    std::cout << std::string(indent, ' ') << "]";
  }
  void operator()(const BencodeDict& dict) const {
    std::cout << "{\n";
    for (const auto& [key, val_ptr] : dict) {
      std::cout << std::string(indent + 2, ' ') << "\"" << key << "\": ";
      if (key == "pieces") {
        std::cout << "\"(... pieces data redacted ...)\"";
      } else if (key == "peers") {
        printBencodeValue(*val_ptr, indent + 2); 
      } else {
        printBencodeValue(*val_ptr, indent + 2); // Recursive call
      }
      std::cout << ",\n";
    }
    std::cout << std::string(indent, ' ') << "}";
  }
};

/**
 * @brief Pretty-prints a parsed BencodeValue.
 */
static void printBencodeValue(const BencodeValue& bv, int indent) {
  std::visit(BencodePrinter{indent}, bv.value);
}

/**
 * @brief Generates a 20-byte, BEP 20 compliant peer_id.
 * * Format: "-GT0001-<12 random digits>"
 * 
 * BitTorrent specification has a page on "Peer ID Conventions"
 * https://www.bittorrent.org/beps/bep_0020.html
 * 
 * Using Mainline style:
 * - Start with dash
 * 
 * GT Two characters to identify client implementation (GoTorrent)
 * 
 * 0001 number representing the version number
 * 
 * - End main with dash
 * 
 * <12 random digits> a string of 12 bytes each a digit
 * Using digits to avoid escaping characters
 * 
 * @return A 20-byte string.
 */
static std::string generatePeerId() {
  const std::string clientPrefix = "-GT0001-";
  std::string peerId = clientPrefix;
  peerId.reserve(20); 

  std::random_device rd;  
  std::mt19937 gen(rd()); 
  std::uniform_int_distribution<> dis(0, 9); 

  for (int i = 0; i < 12; ++i) {
    peerId += std::to_string(dis(gen));
  }
  
  return peerId;
}


// --- Client Class Implementation ---

Client::Client(asio::io_context& io_context, std::string torrentFilePath)
  : io_context_(io_context),
    torrentFilePath_(std::move(torrentFilePath)),
    port_(6881) // Hardcode port for now
{
}

// Defining the constructor here as default
// allows for forward declaration of PeerConnection
// in client.h.
Client::~Client() = default;

void Client::run() {
  loadTorrent();
  requestPeers();
  connectToPeers();
  
  // Run the Asio event loop, processing all async operations.
  // This blocks until all work is finished.
  // (Which should never happen since 
  // it calls async reads constantly.)
  std::cout << "\n--- STARTING ASIO EVENT LOOP ---" << std::endl;
  try {
    io_context_.run();
  } catch (const std::exception& e) {
    std::cerr << "Event loop error: " << e.what() << std::endl;
  }
  std::cout << "--- ASIO EVENT LOOP ENDED ---" << std::endl;
}

void Client::loadTorrent() {
  // Parse torrent file
  torrent_ = parseTorrentFile(torrentFilePath_);

  // Generate a peer_id
  peerId_ = generatePeerId();

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
  totalLength_ = getTotalLength(*infoDict); 
  std::cout << "Torrent piece length: " << pieceLength_ << std::endl;
  std::cout << "Torrent total length: " << totalLength_ << std::endl;

  // Each bit in the bitfield corresponds to a piece being recieved
  //
  // Need amount of pieces / 8 (bits in byte) rounded up to hold
  // trailing bits
  size_t bitfieldSize = (numPieces_ + 7) / 8; // ceil(numPieces / 8.0)

  // Create and send our bitfield (all zeros, we have no pieces)
  myBitfield_.resize(bitfieldSize, 0);

  // Print the hash
  std::cout << "--- INFO HASH ---" << std::endl;
  std::cout << std::hex << std::setfill('0');
  for (unsigned char c : torrent_.infoHash) {
    std::cout << std::setw(2) << static_cast<int>(c);
  }
  std::cout << std::dec << std::endl;
}

void Client::requestPeers() {
  // Get announce URL
  auto& announceVal = torrent_.mainData.at("announce")->value;
  std::string announceUrl;
  if (auto* url = std::get_if<std::string>(&announceVal)) {
    announceUrl = *url;
  } else {
    throw std::runtime_error("Announce URL is not a string.");
  }

  // Get total length (for initial "left")
  auto& infoVal = torrent_.mainData.at("info")->value;
  long long totalLength = 0;
  if (auto* infoDict = std::get_if<BencodeDict>(&infoVal)) {
    totalLength = getTotalLength(*infoDict);
  } else {
    throw std::runtime_error("Torrent 'info' field is not a dictionary.");
  }

  // Build the Tracker GET URL
  std::string trackerUrl = buildTrackerUrl(
    announceUrl,
    torrent_.infoHash,
    peerId_,
    port_,
    0,    // uploaded
    0,    // downloaded
    totalLength,
    1     // compact
  );

  // Print request details
  std::cout << "\n--- PREPARING TRACKER REQUEST ---" << std::endl;
  std::cout << "Peer ID: " << peerId_ << std::endl;
  std::cout << "Announce URL: " << announceUrl << std::endl;
  std::cout << "Total Length (left): " << totalLength << std::endl;
  std::cout << "\n--- COMPLETE TRACKER URL ---" << std::endl;
  std::cout << trackerUrl << std::endl;

  // Send request
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
        peers_ = parseCompactPeers(*peersStr); // Store peers in member
        for (const auto& peer : peers_) {
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

void Client::connectToPeers() {
  if (peers_.empty()) {
    std::cout << "\nNo peers found. Exiting." << std::endl;
    return;
  }

  std::cout << "\n--- CONNECTING TO PEERS ---" << std::endl;

  // Try to connect to every peer
  for (const auto& peer : peers_) {
    try {
      // Create the connection object
      auto peerConn = std::make_shared<PeerConnection>(io_context_, peer.ip, peer.port);

      std::cout << "Connecting to " << peer.ip << ":" << peer.port << std::endl;
      
      // Attempt to connect
      // This throws error on failure
      peerConn->connect();

      // Attempt to handshake
      // This throws error on failure
      std::vector<unsigned char> peerResponseId = peerConn->performHandshake(
        torrent_.infoHash,
        peerId_
      );

      std::cout << "  Handshake successful with " << peer.ip << ". Peer ID: ";
      for (unsigned char c : peerResponseId) {
        if (std::isprint(c)) std::cout << c;
        else std::cout << '.';
      }
      std::cout << std::endl;

      // Send bitfield
      peerConn->sendBitfield(myBitfield_);

      // Start asyncronous message loop 
      peerConn->start(pieceLength_, totalLength_, numPieces_, &myBitfield_, &pieceHashes_);

      // Add to list of active connections
      peerConnections_.push_back(std::move(peerConn));

    } catch (const std::exception& e) {
      // If one peer fails, just print an error and continue with the next one
      std::cerr << "  Failed to connect to peer " << peer.ip << ": " << e.what() << std::endl;
    }
  }

  std::cout << "--- CONNECTED TO " << peerConnections_.size() << " PEERS ---" << std::endl;
}