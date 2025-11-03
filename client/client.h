#ifndef CLIENT_H
#define CLIENT_H

#include "torrent.h" // For TorrentData
#include "tracker.h" // For Peer
#include <boost/asio.hpp>
#include <string>
#include <vector>

namespace asio = boost::asio;

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

  // Member variables
  asio::io_context& io_context_;
  std::string torrentFilePath_;
  
  TorrentData torrent_;
  std::string peerId_;
  std::vector<Peer> peers_;
  long long port_;
};

#endif // CLIENT_H

