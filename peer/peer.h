#ifndef PEER_H
#define PEER_H

#include <boost/asio.hpp> // Main Asio header
#include <string>
#include <vector>

// Use a convenience namespace
namespace asio = boost::asio;
using asio::ip::tcp;

/**
 * @brief Manages a single TCP connection to a BitTorrent peer.
 */
class PeerConnection {
public:
  /**
   * @brief Constructs a peer connection.
   * @param io_context The single, shared Asio io_context.
   * @param peer_ip The IP address of the peer to connect to.
   * @param peer_port The port of the peer to connect to.
   */
  PeerConnection(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port);

  /**
   * @brief Attempts to synchronously connect to the peer.
   * @throws std::exception on connection failure.
   */
  void connect();

  /**
   * @brief Performs the 68-byte BitTorrent handshake.
   * @param infoHash The 20-byte info_hash.
   * @param peerId Our 20-byte peer_id.
   * @return The 20-byte peer_id from the other client, or an empty vector on failure.
   */
  std::vector<unsigned char> performHandshake(
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId
  );

private:
  std::string ip_;
  std::string port_str_;
  tcp::socket socket_; // The socket for this connection
};

#endif // PEER_H
