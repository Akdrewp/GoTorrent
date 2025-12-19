#ifndef TORRENT_SESSION_H
#define TORRENT_SESSION_H

#include "ITorrentSession.h"
#include "ITorrentStorage.h"
#include "httpTrackerClient.h"
#include "torrent.h"
#include "tracker.h"
#include "peer.h"
#include <boost/asio.hpp>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <set>

namespace asio = boost::asio;
using asio::ip::tcp;

/**
 * @brief Manages the entire lifecycle of a single torrent download.
 *
 * This class orchestrates tracker communication, file management,
 * and peer coordination.
 */
class TorrentSession : public ITorrentSession, public std::enable_shared_from_this<TorrentSession> {
public:
  /**
   * @brief Constructs the TorrentSession.
   * @param io_context A reference to the main ASIO io_context.
   * @param torrent The parsed torrent data.
   * @param peerId The client's global 20-byte peer ID.
   * @param port The port the client is listening on.
   * @param trackerClient the tracker send request to use
   */
  TorrentSession(
    asio::io_context& io_context, 
    TorrentData torrent,
    std::string& peerId,
    unsigned short port,
    std::shared_ptr<ITrackerClient> trackerClient,
    std::shared_ptr<ITorrentStorage> storage
  );

  /**
   * @brief Starts the session
   * 
   * Loads torrent info, contacts and connects to peer
   */
  void start();

  /**
   * @brief Handles a new inbound connection from the Client.
   * @param socket The newly accepted socket.
   */
  void handleInboundConnection(tcp::socket socket);

  // --- ITorrenSession Implementation

  // --- PUBLIC CALLBACKS (called by Peer) ---

  void onPieceCompleted(size_t pieceIndex, std::vector<uint8_t> data);
  void onBitfieldReceived(std::shared_ptr<Peer> peer, std::vector<uint8_t> bitfield);
  void onHaveReceived(std::shared_ptr<Peer> peer, size_t pieceIndex);
  std::optional<size_t> assignWorkForPeer(std::shared_ptr<Peer> peer);
  void unassignPiece(size_t pieceIndex);
  
  // --- PUBLIC GETTERS (called by Peer) ---

  long long getPieceLength() const { return pieceLength_; }
  long long getTotalLength() const { return totalLength_; }
  const std::vector<uint8_t>& getBitfield() const { return myBitfield_; }
  
  bool clientHasPiece(size_t pieceIndex) const;
  const char* getPieceHash(size_t pieceIndex) const;
  void updateMyBitfield(size_t pieceIndex);

private:
  /**
   * @brief Loads torrent info and opens the output file.
   */
  void loadTorrentInfo();

  /**
   * @brief Contacts the tracker to get a list of peers.
   */
  void requestPeers();

  /**
   * @brief Attempts to connect to the peers from the peer list.
   */
  void connectToPeers();

  /**
   * @brief Checks if peer has pieces we want and tells peer.
   */
  void checkIfPeerIsInteresting(std::shared_ptr<Peer> peer);

  // --- Helper Functions ---

  // --- Helper Functions END ---


  // --- Client Server Components ---
  asio::io_context& io_context_;
  std::string& peerId_; // Client's peerId
  unsigned short port_; // Port we are listening on
  
  // --- Torrent Info ---
  TorrentData torrent_;
  long long pieceLength_ = 0;
  long long totalLength_ = 0;
  size_t numPieces_ = 0;
  std::vector<uint8_t> myBitfield_; // Client bitfield
  std::string pieceHashes_;         // Raw 20-byte SHA1 hashes

  // Dependencies
  std::shared_ptr<ITrackerClient> trackerClient_;
  std::shared_ptr<ITorrentStorage> storage_;

  // --- File Info ---
  std::string outputFilename_;
  std::fstream outputFile_;

  // --- Peer & Piece Management ---
  std::set<size_t> assignedPieces_;
  std::vector<PeerInfo> trackerPeers_;
  std::vector<std::shared_ptr<Peer>> activePeers_;

  // Stores the count of how many peers have each piece
  // Used for requesting rarests pieces first
  std::vector<size_t> pieceAvailability_;
};

#endif // TORRENT_SESSION_H