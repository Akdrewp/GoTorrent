#include "torrentSession.h"
#include "bencode.h" // For BencodeValue, printBencodeValue
#include <iostream>
#include <variant>    // For std::visit, std::get_if
#include <stdexcept>  // For std::runtime_error
#include <functional> // For std::bind, std::placeholders
#include <random> // For random
#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

// --- TorrentSession class ---

TorrentSession::TorrentSession(
  asio::io_context& io_context, 
  TorrentData torrent,
  std::string peerId,
  unsigned short port,
  std::shared_ptr<ITrackerClient> trackerClient,
  std::shared_ptr<IPieceRepository> repo,
  std::shared_ptr<IPiecePicker> picker,
  std::shared_ptr<IChokingAlgorithm> choker
) : io_context_(io_context),
    torrent_(std::move(torrent)),
    peerId_(std::move(peerId)),
    port_(port),
    trackerClient_(std::move(trackerClient)),
    repo_(std::move(repo)),
    picker_(std::move(picker)),
    choker_(std::move(choker)),
    chokingTimer_(io_context)
{
  if (!repo_ || !picker_) {
    throw std::runtime_error("TorrentSession requires valid Repo and Picker");
  }
}

/**
 * @brief Starts the session
 * Loads torrent info, contacts and connects to peer
 */
void TorrentSession::start() {
  spdlog::info("--- Starting Torrent Session ---");
  
  // Initialize the Repository (Load existing data, verify files)
  // TODO CHANGE PATH
  std::string DOWNLOAD_PATH = "./downloads"; 
  repo_->initialize(DOWNLOAD_PATH);

  startChokingTimer();

  requestPeers();
  connectToPeers();
}

/**
 * @brief Starts choking algorithm every 10 seconds
 * 
 * Sets an asyncronous timer for 10 seconds then calls
 * choking algorithm every 10 seconds
 * 
 * No infinite stack recursion since asyncronous
 */
void TorrentSession::startChokingTimer() {
  // Set timer for 10 seconds
  chokingTimer_.expires_after(std::chrono::seconds(10));

  chokingTimer_.async_wait([self = shared_from_this()](const std::error_code& ec) {
    if (ec) return; // Timer cancelled

    self->choker_->rechoke(self->activePeers_);

    self->startChokingTimer();
  });
}

/**
 * @brief Requests compact peer list from tracker
 * 
 * 1. Build tracker url from torrentData
 * 2. Send request
 * 3. Parse response
 * 4. Load peer list into trackerPeers_
 */
void TorrentSession::requestPeers() {

  // 1. Build tracker url from torrentData
  std::string announceUrl = torrent_.mainData.at("announce")->get<std::string>();
  long long totalLength = repo_->getTotalLength();
  std::string trackerUrl = buildTrackerUrl(
    announceUrl,
    torrent_.infoHash,
    peerId_,
    port_,
    0,    // uploaded
    0,    // downloaded (TODO: get from repo)
    totalLength,
    1     // compact
  );

  // 2. Print and send request 
  spdlog::info("--- PREPARING TRACKER REQUEST ---");
  spdlog::info("Announce URL: {}", announceUrl);
  spdlog::info("Total Length (left): {}", totalLength);

  spdlog::info("--- SENDING REQUEST TO TRACKER ---");
  std::string trackerResponse = trackerClient_->sendRequest(trackerUrl);
  spdlog::info("Tracker raw response size: {} bytes", trackerResponse.size());

  // 3. Parse tracker response
  spdlog::info("--- PARSING TRACKER RESPONSE ---");
  size_t index = 0;
  std::vector<char> responseBytes(trackerResponse.begin(), trackerResponse.end());
  BencodeValue parsedResponse = parseBencodedValue(responseBytes, index);
  
  printBencodeValue(parsedResponse);
  std::cout << std::endl;

  // 4. Get and load peer list into trackerPeers_
  spdlog::info("--- PARSED PEER LIST ---");
  const auto& respDict = parsedResponse.get<BencodeDict>();

  if (respDict.count("failure reason")) {
    std::string failure = respDict.at("failure reason")->get<std::string>();
    throw std::runtime_error("Tracker error: " + failure);
  }

  if (respDict.count("peers")) {
    // Assume compact
    std::string peersStr = respDict.at("peers")->get<std::string>();
    trackerPeers_ = parseCompactPeers(peersStr);
  }
}

// --- connectToPeers ---

/**
 * @brief Creates a peer and injects dependencies
 * 
 * Helper for connectToPeers
 * Creates the PeerConnection objects and injects
 * Connection, pieceRepository, and piecePicker into Peer
 */
static std::shared_ptr<Peer> initPeer(
    asio::io_context& io_context, 
    std::string peer_ip, 
    uint16_t peer_port,
    std::shared_ptr<IPieceRepository> repo,
    std::shared_ptr<IPiecePicker> picker
) {
  auto conn = std::make_shared<PeerConnection>(io_context, peer_ip, peer_port);
  
  auto peer = std::make_shared<Peer>(conn, peer_ip, repo, picker);

  return peer;
}

/**
 * @brief Attempts to connect to peers on trackerList
 * 
 * For every peer in trackerPeers_
 *  1. Create peer
 *  2. Starts peer connection
 *  3. Add peer to peer list
 */
void TorrentSession::connectToPeers() {
  if (trackerPeers_.empty()) {
    spdlog::warn("No peers found from tracker.");
    return;
  }

  spdlog::info("--- CONNECTING TO PEERS ---");
  using namespace std::placeholders; // for _1, _2

  // Connect to every peer in tracker list
  for (const auto& peerInfo : trackerPeers_) {

    // 1. Create peer object
    auto peer = initPeer(io_context_, peerInfo.ip, peerInfo.port, repo_, picker_);

    spdlog::info("Attempting async connect to {}:{}", peerInfo.ip, peerInfo.port);

    // 2. Starts peer connection
    peer->startAsOutbound(
      torrent_.infoHash,
      peerId_,
      shared_from_this() // Pass self as session
    );

    // 3. Add peer to peer lists
    activePeers_.push_back(peer);
  }
  
  spdlog::info("--- CONNECTED TO {} PEERS ---", activePeers_.size());
}

void TorrentSession::handleInboundConnection(tcp::socket socket) {
  spdlog::info("--- INBOUND CONNECTION from {} ---", socket.remote_endpoint().address().to_string());
  
  // Create Connection
  auto conn = std::make_shared<PeerConnection>(io_context_, std::move(socket));
  
  // Create peer with injected dependencies
  auto peer = std::make_shared<Peer>(conn, conn->get_ip(), repo_, picker_);

  // Start the inbound connection process
  // This waits for the peer's handshake, then reply
  peer->startAsInbound(
    torrent_.infoHash,
    peerId_,
    shared_from_this() // Pass self as session
  );
  // Add peer to peer lists
  activePeers_.push_back(peer);
}

/**
 * @brief CALLBACK for Peer
 * 
 * 1. Find peer in activePeers_.
 * 2. Remove from activePeers_.
 */
void TorrentSession::onPeerDisconnected(std::shared_ptr<Peer> peer) {
  // 1. Find the peer in the vector
  auto it = std::find(activePeers_.begin(), activePeers_.end(), peer);
  
  // 2. If found remove
  if (it != activePeers_.end()) {
    spdlog::info("[{}] Peer disconnected. Removing from active list. Remaining: {}", 
    peer->getIp(), activePeers_.size() - 1);
        
    activePeers_.erase(it);
  } else {
    spdlog::warn("onPeerDisconnected called for peer that was not found in active list.");
  }
}
