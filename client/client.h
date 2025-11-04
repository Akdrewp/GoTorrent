#ifndef CLIENT_H
#define CLIENT_H

#include "torrent.h" // For TorrentData
#include "tracker.h" // For Peer
#include <boost/asio.hpp>
#include <string>
#include <vector>

namespace asio = boost::asio;

// Forward declrationg defined in peer.h
class PeerConnection;

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
   * @brief Sends bitfields to connected peer.
   */
  void sendBitfieldToPeer();

  /**
   * @brief Sends our bitfield and starts the main loop for peer messages.
   */
  void startMessageLoop();

  // Member variables
  asio::io_context& io_context_;
  std::string torrentFilePath_;
  
  TorrentData torrent_;
  std::string peerId_;
  std::vector<Peer> peers_;
  long long port_;
  // Stores our active connection
  // This is only one connection at a time for now.
  // Later on convert to vector or combine with peers_
  std::unique_ptr<PeerConnection> peerConn_;
};

#endif // CLIENT_H

