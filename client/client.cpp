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
  startMessageLoop();
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

  // Each piece has a hash in "pieces" each with length 20
  // Amount of pieces is length of "pieces" / 20
  numPieces_ = piecesStr->length() / 20;

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

  std::cout << "\n--- CONNECTING TO PEER ---" << std::endl;
  // Let's try to connect to the first peer in the list
  const auto& firstPeer = peers_[0];
  try {
    // Create the connection object
    peerConn_ = std::make_unique<PeerConnection>(io_context_, firstPeer.ip, firstPeer.port);

    // Attempt to connect
    peerConn_->connect();

    // Attempt to handshake
    std::vector<unsigned char> peerResponseId = peerConn_->performHandshake(
      torrent_.infoHash,
      peerId_
    );

    std::cout << "Received peer ID: ";
    for (unsigned char c : peerResponseId) {
      if (std::isprint(c)) std::cout << c;
      else std::cout << '.';
    }
    std::cout << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "Peer connection failed: " << e.what() << std::endl;
  }
}

void Client::startMessageLoop() {
  if (!peerConn_) {
    std::cout << "\nNo active peer connection. Skipping message loop." << std::endl;
    return;
  }

  std::cout << "\n--- STARTING MESSAGE LOOP ---" << std::endl;
  try {

    // Starting connection
    peerConn_->sendBitfield(myBitfield_);

    // Read incoming message response
    std::cout << "Waiting for peer messages..." << std::endl;

    /**
     * Main loop
     * 
     * Read Message
     * 
     * Update state based of message
     * 
     * Act on updated state
     * 
     * @todo
     * Currently this still does wait for a message before
     * doing an action
     */
    while (true) {
      // Read a message (blocking)
      PeerMessage msg = peerConn_->readMessage();

      // Handle the message (update state)
      handleMessage(*peerConn_, msg);

      // Do an action (make decisions based on new state)
      doAction(*peerConn_);
    }

  } catch (const std::exception& e) {
    std::cerr << "Message loop failed: " << e.what() << std::endl;
    peerConn_ = nullptr; // Connection failed, reset
  }
}

/**
 * @brief Main message router
 * @param peer The peer that sent the message.
 * @param msg The message received from the peer.
 */
void Client::handleMessage(PeerConnection& peer, const PeerMessage& msg) {
  switch (msg.id) {
    case 0: // choke
      handleChoke(peer);
      break;
    case 1: // unchoke
      handleUnchoke(peer);
      break;
    case 4: // have
      handleHave(peer, msg);
      break;
    case 5: // bitfield
      handleBitfield(peer, msg);
      break;
    case 7: // piece
      // TODO: Implement handlePiece
      std::cout << "Received PIECE (TODO: Handle this)" << std::endl;
      break;
    default:
      std::cout << "Received unhandled message. ID: " << (int)msg.id << std::endl;
  }
}

/**
 * --- State Handlers ---
 * 
 * Should deal with a state and do the corresponding action
 * 
 * Possible to change state as well
 * 
 * @TODO
 * Should be using buffers to queue messages
 */

/**
 * @brief Makes decisions based on the current peer state.
 * This is the "brain" of the client's interaction.
 */
void Client::doAction(PeerConnection& peer) {

  // Action 1: Check if we should be interested in this peer.
  checkAndSendInterested(peer);

  // Action 2: Check if we can and should request a piece.
  if (peer.amInterested_ && !peer.peerChoking_) {
    // We are interested, and the peer is not choking us
    // request a piece.
    requestPiece(peer);
  }
}

/**
 * @brief Finds and requests the next available piece/block.
 * * TODO: This is a simple placeholder. A real client needs
 * to manage piece/block state (e.g., what's requested,
 * what's in-flight, what's completed).
 */
void Client::requestPiece(PeerConnection& peer) {
  // TODO: Reqrite currently
  // re-requests the same piece every loop.
  // Maybe have a buffer for the peer of requests sent.

  // Also requests sent before being choked should be void


  // Check if a peer has a piece we don't have
  // If interested and not choking then request
  for (size_t i = 0; i < numPieces_; ++i) {
    bool peerHas = peer.hasPiece(i);
    
    // Check our bit
    size_t my_byte_index = i / 8;
    uint8_t my_bit_index = 7 - (i % 8);
    bool clientHas = (myBitfield_[my_byte_index] & (1 << my_bit_index)) != 0;

    if (peerHas && !clientHas) {
      // Found a piece we want
      // TODO: Use buffer this requests already requested pieces ):
      // For now, let's just request the first block (16KB)
      // of this piece and then stop.
      

      std::cout << "--- ACTION: Requesting piece " << i << " ---" << std::endl;

      // TODO: Get piece length and handle multiple blocks.
      // For now, just request the first block.
      peer.sendRequest(
        static_cast<uint32_t>(i), // pieceIndex
        0,                        // begin
        BLOCK_SIZE                // length
      );

      // Explicit return for now stop stack smashing?
      return;
    }
  }
}


/** 
 * --- Message Handlers ---
 *  
 * State Updaters
 * 
 * Should only update state
 * 
 * What's done with the state after handling a message
 * should be handled by doAction (name tentattive)
 */

void Client::handleChoke(PeerConnection& peer) {
  std::cout << "Received CHOKE" << std::endl;
  peer.peerChoking_ = true;
}

void Client::handleUnchoke(PeerConnection& peer) {
  std::cout << "Received UNCHOKE" << std::endl;
  peer.peerChoking_ = false;

  // The peer is no longer choking us. If we are interested,
  // we should start sending piece requests.
  if (peer.amInterested_) {
    std::cout << "Peer unchoked us and we are interested. Time to request pieces!" << std::endl;
    // TODO: Add logic to pick a piece and send a request
    // peer.sendRequest(...);
  }
}

void Client::handleHave(PeerConnection& peer, const PeerMessage& msg) {
  if (msg.payload.size() != 4) {
    std::cerr << "Invalid HAVE message payload size: " << msg.payload.size() << std::endl;
    return;
  }
  
  // Payload is the 4-byte piece index in network byte order
  uint32_t pieceIndex;
  memcpy(&pieceIndex, msg.payload.data(), 4);
  pieceIndex = ntohl(pieceIndex);

  std::cout << "Received HAVE for piece " << pieceIndex << std::endl;
  
  // Update the peer's bitfield
  peer.setHavePiece(pieceIndex);

  // Check if this new piece makes us interested
  checkAndSendInterested(peer);
}

void Client::handleBitfield(PeerConnection& peer, const PeerMessage& msg) {
  std::cout << "Received BITFIELD (" << msg.payload.size() << " bytes)" << std::endl;
  peer.bitfield_ = msg.payload;

  // TODO: Add logic to compare our bitfield to the peer's.
  // For now, let's just assume they have something we want
  // and send an INTERESTED message.
  if (!peer.amInterested_) {
    checkAndSendInterested(peer);
  }
}

/** 
 * @brief Checks if we are interested in the peer and sends an
 * Interested message (ID 2) if we aren't already.
 * 
 * We are interested if the peer has a piece we don't
 */
void Client::checkAndSendInterested(PeerConnection& peer) {
  // If we are already interested, nothing to do
  if (peer.amInterested_) {
    return;
  }

  // Check if the peer has *any* piece that we *don't* have.
  for (size_t i = 0; i < numPieces_; ++i) {
    bool peerHas = peer.hasPiece(i);
    
    // Check our bit
    size_t my_byte_index = i / 8;
    uint8_t my_bit_index = 7 - (i % 8);
    bool iHave = (myBitfield_[my_byte_index] & (1 << my_bit_index)) != 0;

    if (peerHas && !iHave) {
      std::cout << "Peer has piece " << i << " which we don't. Sending INTERESTED." << std::endl;
      peer.sendInterested();
      peer.amInterested_ = true;
      return; // Found a piece, sent message, we're done.
    }
  }

  std::cout << "Peer bitfield contains no pieces we need." << std::endl;
}