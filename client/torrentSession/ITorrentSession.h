#ifndef I_TORRENT_SESSION_H
#define I_TORRENT_SESSION_H

#include <memory>

// Forward declaration
class Peer;

/**
 * @brief Interface for session for testing purposes
 */
class ITorrentSession {
public:
  virtual ~ITorrentSession() = default;

  /**
   * @brief Called by a Peer when it disconnects or encounters a fatal error.
   * The session should remove the peer from the active list.
   */
  virtual void onPeerDisconnected(std::shared_ptr<Peer> peer) = 0;

};

#endif // I_TORRENT_SESSION_H