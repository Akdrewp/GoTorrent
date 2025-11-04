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

  /**
   * @brief Main message router. Called by startMessageLoop.
   * @param peer The peer that sent the message.
   * @param msg The message received from the peer.
   */
  void handleMessage(PeerConnection& peer, const PeerMessage& msg);

  // --- Message Handlers ---

  /** 
   * @brief Handles a Choke message (ID 0) 
  */
  void handleChoke(PeerConnection& peer);
  /** 
   * @brief Handles an Unchoke message (ID 1)
  */
  void handleUnchoke(PeerConnection& peer);
  /** 
   * @brief Handles a Have message (ID 4) 
  */
  void handleHave(PeerConnection& peer, const PeerMessage& msg);
  /** 
   * @brief Handles a Bitfield message (ID 5) 
  */
  void handleBitfield(PeerConnection& peer, const PeerMessage& msg);

  /** 
   * @brief Checks if we are interested in the peer and sends an
   * Interested message (ID 2) if we aren't already.
   * 
   * We are interested if the peer has a piece we don't
   */
  void checkAndSendInterested(PeerConnection& peer);

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

  size_t numPieces_ = 0;
  std::vector<uint8_t> myBitfield_; // What pieces we have
};

#endif // CLIENT_H

