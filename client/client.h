#ifndef CLIENT_H
#define CLIENT_H

#include "torrent.h" // For TorrentData
#include "tracker.h" // For Peer
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp> // For tcp::acceptor
#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace asio = boost::asio;
using asio::ip::tcp;

// Forward declrationg defined in peer.h
class PeerConnection;

// Forward declration defined in peer.h
struct PeerMessage;

/**
 * @brief Main BitTorrent client class.
 *
 * Manages the client state, from parsing the torrent to
 * connecting to trackers and peers.
 */
class Client {
public:
  /**
   * @brief Constructs the Client.
   * @param io_context A reference to the main ASIO io_context.
   * @param torrentFilePath The path to the .torrent file.
   */
  Client(asio::io_context& io_context, std::string torrentFilePath);

  /**
   * @brief Runs the main client logic.
   *
   * This function orchestrates loading the torrent, contacting the
   * tracker, and connecting to peers.
   * @throws std::runtime_error on any fatal error.
   */
  void run();

  /**
   * @brief Destructor.
   *
   * Defined in the .cpp file to allow std::unique_ptr
   * to work with a forward-declared PeerConnection.
   */
  ~Client();

private: 

  /**
   * @TODO Redo comments for multiple peers
   */
  /**
   * @brief Loads torrent file, parses it, and generates our peer_id.
   */
  void loadTorrent();

  /**
   * @brief Contacts the tracker to get a list of peers.
   */
  void requestPeers();

  /**
   * @brief Attempts to connect to the peers from the peer list.
   */
  void connectToPeers();

  /**
   * @brief Starts the TCP acceptor to listen for new peers (Inbound).
   */
  void startAccepting();

  /**
   * @brief Callback function to handle a new inbound peer connection.
   * @param ec An error code from the accept operation.
   * @param socket The new socket for the connected peer.
   */
  void handleAccept(const boost::system::error_code& ec, tcp::socket socket);


  // --- Torrent Info ---
  long long pieceLength_ = 0;
  long long totalLength_ = 0;
  size_t numPieces_ = 0;
  std::vector<uint8_t> myBitfield_; // Client bitfield
  std::string pieceHashes_;         // Raw 20-byte SHA1 hashes

  // --- External Variables ---
  asio::io_context& io_context_;
  std::string torrentFilePath_;
  
  TorrentData torrent_;

  // --- Server Variables ---
  std::string peerId_;
  std::vector<Peer> peers_;
  long long port_;

  tcp::acceptor acceptor_;

  // Stores the peer connections
  // @TODO combine Peer and PeerConnection Object?
  // Stores a vector of pointers to each connection
  std::vector<std::shared_ptr<PeerConnection>> peerConnections_;
};

#endif // CLIENT_H

