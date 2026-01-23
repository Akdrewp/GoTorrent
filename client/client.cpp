#include "client.h"
#include "peer.h"     // For Peer
#include "bencode.h"  // For BencodeValue, parseBencodedValue
#include "httpTrackerClient.h" // For Tracker
#include "diskTorrentStorage.h" // For storage
#include "pieceRepository.h" // For PieceRepository
#include "piecePicker.h" // For PiecePicker
#include "titForTatChoking.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/basic_file_sink.h"
#include <variant>     // For std::visit, std::get_if
#include <stdexcept>   // For std::runtime_error
#include <random>      // For std::random_device, std::mt19937
#include <functional>  // For std:bind

// --- Constants ---
static constexpr uint32_t BLOCK_SIZE = 16384;

/**
 * @brief Generates a 20-byte, BEP 20 compliant peer_id.
 * * Format: "-GT0001-<12 random digits>"
 * * BitTorrent specification has a page on "Peer ID Conventions"
 * https://www.bittorrent.org/beps/bep_0020.html
 * * Using Mainline style:
 * - Start with dash
 * * GT Two characters to identify client implementation (GoTorrent)
 * * 0001 number representing the version number
 * * - End main with dash
 * * <12 random digits> a string of 12 bytes each a digit
 * Using digits to avoid escaping characters
 * * @return A 20-byte string.
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


// Initialize logging for app
void initLogging() {
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("bittorrent.log", true);
  
  // Combine sinks into one logger
  std::vector<spdlog::sink_ptr> sinks {console_sink, file_sink};
  auto logger = std::make_shared<spdlog::logger>("global", sinks.begin(), sinks.end());
  
  // Set pattern: [Time] [Level] Message
  logger->set_pattern("[%H:%M:%S] [%^%l%$] %v");
  
  // REGISTER IT AS GLOBAL
  spdlog::set_default_logger(logger);
}

void Client::run() {

  initLogging();

  // Parse the torrent file
  TorrentData torrent = parseTorrentFile(torrentFilePath_);

  // Generate a peer_id
  peerId_ = generatePeerId();

  // Tracker
  auto trackerClient = std::make_shared<HttpTrackerClient>(); 

  // Storage
  auto storage = std::make_shared<DiskTorrentStorage>();

  // Piece Repository (Handles data integrity and storage)
  // Wraps storage and handles hashing/bitfields
  auto repo = std::make_shared<PieceRepository>(storage, torrent);

  // Piece Picker (Handles strategy and piece assignment)
  // Calculate total pieces to initialize picker
  const auto& infoDict = torrent.mainData.at("info")->get<BencodeDict>();
  const std::string& piecesStr = infoDict.at("pieces")->get<std::string>();
  size_t numPieces = piecesStr.length() / 20;
  
  // Picker
  auto picker = std::make_shared<PiecePicker>(numPieces);

  auto choker = std::make_shared<TitForTatChoking>();

  TorrentData sessionTorrent = parseTorrentFile(torrentFilePath_);

  // Create the TorrentSession to manage this download
  session_ = std::make_shared<TorrentSession>(
    io_context_, 
    std::move(sessionTorrent), // Pass copy/move of torrent data
    peerId_, 
    port_,
    trackerClient,
    repo,   // Inject Repository
    picker,
    choker
  );

  try {
    session_->start();
  } catch (const std::exception& e) {
    spdlog::error("Failed to start session: {}", e.what());
    return;
  }

  // Start listening for inbound connections
  startAccepting();
  
  // Run the Asio event loop, processing all async operations.
  // This blocks until all work is finished.
  // (Which should never happen since 
  // it calls async reads constantly.)
  spdlog::info("--- STARTING ASIO EVENT LOOP ---");
  try {
    io_context_.run();
  } catch (const std::exception& e) {
    spdlog::error("Event loop error: {}", e.what());
  }
  spdlog::info("--- ASIO EVENT LOOP ENDED ---");
}

/**
 * @brief Starts the TCP acceptor to listen for new peers (Inbound).
 */
void Client::startAccepting() {
  spdlog::info("Waiting for inbound connections...");
  
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
      spdlog::error("Accepted connection but no active session.");
      socket.close();
    }
  } else {
    spdlog::error("Acceptor error: {}", ec.message());
  }

  // Listen for the next connection
  startAccepting();
}