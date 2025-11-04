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

  /**
   * @brief Constructs and sends a Bitfield message (ID=5) to the peer.
   * @param bitfield The raw bytes of the bitfield.
   * @throws std::runtime_error on send failure.
   */
void PeerConnection::sendBitfield(const std::vector<uint8_t>& bitfield) {
  // A bitfield message has ID = 5
  std::vector<unsigned char> payload(bitfield.begin(), bitfield.end());
  sendMessage(5, payload);
  std::cout << "Sent bitfield (" << payload.size() << " bytes) to " << ip_ << std::endl;
}

/**
 * @brief Reads a single peer message from the socket.
 *
 * This is a blocking call. It will read the 4-byte length prefix,
 * then read the full message payload (ID + data).
 *
 * @return A PeerMessage struct.
 * @throws std::runtime_error on read failure.
 */
PeerMessage PeerConnection::readMessage() {
  try {
    // Read the 4-byte length prefix
    uint32_t length_net;
    size_t lenRead = asio::read(socket_, asio::buffer(&length_net, 4), asio::transfer_exactly(4));
    if (lenRead != 4) {
      throw std::runtime_error("Failed to read message length prefix.");
    }

    // Convert from network byte order to host byte order
    uint32_t length = ntohl(length_net);

    // This is a "keep-alive" message
    if (length == 0) {
      return { 0, {} }; // ID 0, empty payload
    }

    // Read the rest of the message (ID + payload)
    std::vector<unsigned char> messageBuffer(length);
    size_t msgRead = asio::read(socket_, asio::buffer(messageBuffer), asio::transfer_exactly(length));
    if (msgRead != length) {
      throw std::runtime_error("Failed to read full message payload.");
    }

    // Separate the ID and the Payload
    PeerMessage msg;
    msg.id = messageBuffer[0]; // First byte is the ID
    msg.payload.assign(messageBuffer.begin() + 1, messageBuffer.end()); // Rest is payload

    return msg;

  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to read message: " + std::string(e.what()));
  }
}

/**
 * @brief Sends a generic message to the peer.
 * Client doesn't use this, each message will have own function
 * Prepends the 4-byte length and 1-byte ID.
 */
void PeerConnection::sendMessage(uint8_t id, const std::vector<unsigned char>& payload) {
  try {
    // Calculate length
    // 1 byte for ID + payload size
    uint32_t length = 1 + payload.size();
    uint32_t length_net = htonl(length); // Convert to network byte order

    // Create a buffer to send
    std::vector<asio::const_buffer> buffers;
    buffers.push_back(asio::buffer(&length_net, 4));
    buffers.push_back(asio::buffer(&id, 1));
    if (!payload.empty()) {
      buffers.push_back(asio::buffer(payload));
    }

    // Send the composed message
    asio::write(socket_, buffers);

  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to send message: " + std::string(e.what()));
  }
}