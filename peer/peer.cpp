#include "peer.h"
#include <iostream>
#include <stdexcept>
#include <cstring> // For memcpy

// Use a convenience namespace
namespace asio = boost::asio;
using asio::ip::tcp;

PeerConnection::PeerConnection(asio::io_context& io_context, std::string peer_ip, uint16_t peer_port)
  : ip_(std::move(peer_ip)),
    port_str_(std::to_string(peer_port)),
    socket_(io_context) // Initialize the socket with the io_context
{
}

/**
 * @brief Constructs a peer connection.
 * @param io_context The single, shared Asio io_context.
 * @param peer_ip The IP address of the peer to connect to.
 * @param peer_port The port of the peer to connect to.
 */
void PeerConnection::connect() {
  try {
    // Resolve the IP and Port
    // This converts the string IP/port into a list of endpoints
    tcp::resolver resolver(socket_.get_executor());
    auto endpoints = resolver.resolve(ip_, port_str_);

    // Connect to the first resolved endpoint
    // This is a synchronous connect call. It will block until
    // it connects or times out.
    asio::connect(socket_, endpoints);

    std::cout << "Successfully connected to peer: " << ip_ << ":" << port_str_ << std::endl;

  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to connect to peer " + ip_ + ":" + port_str_ + ": " + e.what());
  }
}

/**
 * @brief Performs the 68-byte BitTorrent handshake.
 * @param infoHash The 20-byte info_hash.
 * @param peerId Our 20-byte peer_id.
 * @return The 20-byte peer_id from the other client, or an empty vector on failure.
 */
std::vector<unsigned char> PeerConnection::performHandshake(
  const std::vector<unsigned char>& infoHash,
  const std::string& peerId
) {
  /**
   * Construct the 68-byte handshake message
   * handshake: <pstrlen><pstr><reserved><info_hash><peer_id>
   * 
   * <pstrlen (1)>
   * <pstr (19)>
   * <reserved (8)>
   * <info_hash (20)>
   * <peer_id (20)>
   * 
   * (49+len(pstr)) long
   */
  std::vector<unsigned char> handshakeMsg(68);
  
  // pstrlen
  handshakeMsg[0] = 19;
  
  // pstr ("BitTorrent protocol")
  // In version 1.0 of the BitTorrent protocol,
  // pstrlen = 19, and pstr = "BitTorrent protocol".
  const char* pstr = "BitTorrent protocol";
  memcpy(&handshakeMsg[1], pstr, 19);

  // reserved (8 bytes of 0)
  // std::vector initializes to 0, so this is already done.

  // info_hash (20 bytes)
  memcpy(&handshakeMsg[28], infoHash.data(), 20);

  // peer_id (20 bytes)
  memcpy(&handshakeMsg[48], peerId.c_str(), 20);



  // Send the handshake
  std::cout << "Sending handshake to " << ip_ << "..." << std::endl;
  try {
    asio::write(socket_, asio::buffer(handshakeMsg));
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to send handshake: " + std::string(e.what()));
  }

  // Receive the response (must also be 68 bytes)
  std::vector<unsigned char> response(68);
  try {
    size_t bytesRead = asio::read(socket_, asio::buffer(response), asio::transfer_exactly(68));
    if (bytesRead != 68) {
       throw std::runtime_error("Peer did not send full 68-byte handshake.");
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to read handshake: " + std::string(e.what()));
  }

  // Validate the response
  // Check pstr
  if (memcmp(&response[1], pstr, 19) != 0) {
    // Should be "BitTorrent protocol"
    throw std::runtime_error("Peer sent invalid protocol string.");
  }

  // Check info_hash
  if (memcmp(&response[28], infoHash.data(), 20) != 0) {
    // Should be same as clients
    throw std::runtime_error("Peer sent wrong info_hash!");
  }

  // Return the peer's ID
  std::cout << "Handshake successful with " << ip_ << "!" << std::endl;
  std::vector<unsigned char> responsePeerId(20);
  memcpy(responsePeerId.data(), &response[48], 20);
  return responsePeerId;
}
