#include "client.h"
#include "peer.h"     // For Peer
#include "bencode.h"  // For BencodeValue, parseBencodedValue
#include "httpTrackerClient.h" // For Tracker

#include <iostream>    // For std::cout, std::cerr
#include <iomanip>     // For std::setw, std::hex
#include <variant>     // For std::visit, std::get_if
#include <stdexcept>   // For std::runtime_error
#include <random>      // For std::random_device, std::mt19937
#include <functional>  // For std:bind

// --- Constants ---
static constexpr uint32_t BLOCK_SIZE = 16384;

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
    port_(6882), // Hardcode port for now
    acceptor_(io_context, tcp::endpoint(tcp::v4(), port_)) // Initialize the acceptor
{
}

// Defining the destructor here as default
// allows for forward declaration of Peer
// in client.h.
Client::~Client() = default;

void Client::run() {

  // Parse the torrent file
  TorrentData torrent = parseTorrentFile(torrentFilePath_);

  // Generate a peer_id
  peerId_ = generatePeerId();

  // Tracker
  auto trackerClient = std::make_shared<HttpTrackerClient>(); 

  // Create the TorrentSession to manage this download
  session_ = std::make_shared<TorrentSession>(
    io_context_, 
    std::move(torrent), 
    peerId_, 
    port_,
    trackerClient
  );

  try {
    session_->start();
  } catch (const std::exception& e) {
    std::cerr << "Failed to start session: " << e.what() << std::endl;
    return;
  }

  // Start listening for inbound connections
  startAccepting();
  
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

/**
 * @brief Starts the TCP acceptor to listen for new peers (Inbound).
 */
void Client::startAccepting() {
  std::cout << "Waiting for inbound connections..." << std::endl;
  
  // Start an asynchronous accept operation.
  // Completion handler is handleAccept
  acceptor_.async_accept(
    [this](const boost::system::error_code& ec, tcp::socket socket) {
      handleAccept(ec, std::move(socket));
    }
  );
}

/**
 * @brief Callback function to handle a new inbound peer connection.
 */
void Client::handleAccept(const boost::system::error_code& ec, tcp::socket socket) {
  if (!ec) {
    // We got a connection. Pass it to the session to handle.
    if (session_) {
      session_->handleInboundConnection(std::move(socket));
    } else {
      std::cerr << "Accepted connection but no active session." << std::endl;
      socket.close();
    }
  } else {
    std::cerr << "Acceptor error: " << ec.message() << std::endl;
  }

  // Listen for the next connection
  startAccepting();

  // Listen for the next connection, regardless of what happened
  startAccepting();
}