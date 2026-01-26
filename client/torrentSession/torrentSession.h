#ifndef TORRENT_SESSION_H
#define TORRENT_SESSION_H

#include "ITorrentSession.h"
#include "IPieceRepository.h" 
#include "IPiecePicker.h"
#include "httpTrackerClient.h"
#include "torrent.h"
#include "tracker.h"
#include "peer.h" // Required for std::vector<std::shared_ptr<Peer>>
#include "IChokingAlgorithm.h"
#include <boost/asio.hpp>
#include <string>
#include <vector>
#include <memory>
#include <set>

namespace asio = boost::asio;
using asio::ip::tcp;

/**
 * @brief Manages the entire lifecycle of a single torrent download.
 *
 * Loads a torrent file, contacts, trackers and gets peer list
 * Creates peers and injects them with piecePicker and pieceRepository
 */
class TorrentSession : public ITorrentSession, public std::enable_shared_from_this<TorrentSession> {
public:
  /**
   * @brief Constructs the TorrentSession.
   * @param io_context A reference to the main ASIO io_context.
   * @param torrent The parsed torrent data.
   * @param peerId The client's global 20-byte peer ID.
   * @param port The port the client is listening on.
   * @param trackerClient The tracker client to use.
   * @param repo The shared storage and data repository.
   * @param picker The shared piece selection strategy.
   * @param choker The shared peer choking algorithm.
   */
  TorrentSession(
    asio::io_context& io_context, 
    TorrentData torrent,
    std::string peerId,
    unsigned short port,
    std::shared_ptr<ITrackerClient> trackerClient,
    std::shared_ptr<IPieceRepository> repo,
    std::shared_ptr<IPiecePicker> picker,
    std::shared_ptr<IChokingAlgorithm> choker
  );

  /**
   * @brief Starts the session
   * Loads torrent info, contacts tracker, and connects to peers.
   */
  void start();

  /**
   * @brief Handles a new inbound connection from a remote Client.
   * @param socket The newly accepted socket.
   */
  void handleInboundConnection(tcp::socket socket);

  /**
   * @brief Callback for peer
   * 
   * Handles a peer disconnect by removing peer from active peers
   */
  void onPeerDisconnected(std::shared_ptr<Peer> peer) override;

private:
  /**
   * @brief Starts and runs timer for choking algorithm.
   */
  void startChokingTimer();

  /**
   * @brief Contacts the tracker to get a list of peers.
   */
  void requestPeers();

  /**
   * @brief Attempts to connect to the peers from the peer list.
   */
  void connectToPeers();

  // --- Client Server Components ---
  asio::io_context& io_context_;
  std::string peerId_; // Client's peerId
  unsigned short port_; // Port we are listening on
  
  // --- Torrent Info ---
  TorrentData torrent_;

  // --- Dependencies ---
  std::shared_ptr<ITrackerClient> trackerClient_;
  std::shared_ptr<IChokingAlgorithm> choker_;
  asio::steady_timer chokingTimer_;
  
  // These shared resources are injected into every Peer created by this session
  std::shared_ptr<IPieceRepository> repo_;
  std::shared_ptr<IPiecePicker> picker_;

  // --- Peer Management ---
  std::vector<PeerInfo> trackerPeers_;
  std::vector<std::shared_ptr<Peer>> activePeers_;
};

#endif // TORRENT_SESSION_H