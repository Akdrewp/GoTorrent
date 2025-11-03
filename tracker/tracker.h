#ifndef TRACKER_H
#define TRACKER_H

#include <string>
#include <vector>
#include <sstream>   // For std::stringstream
#include <iomanip>   // For std::hex, std::setw
#include <cctype>    // For std::isalnum
#include <cstdint>   // For uint16_t

/**
 * @brief Holds information for a single peer.
 */
struct Peer {
  std::string ip;
  uint16_t port;
};

/**
 * @brief URL-encodes a string of raw bytes.
 *
 * Unreserved characters (a-z, A-Z, 0-9, '-', '_', '.', '~') are left as-is.
 * All other bytes are converted to %XX hex format.
 *
 * @param data The raw binary data (e.g., info_hash or peer_id).
 * @return A URL-safe string.
 */
std::string urlEncode(const std::vector<unsigned char>& data);

/**
 * @brief Overload for URL-encoding a standard string.
 * This treats the string as a sequence of bytes.
 */
std::string urlEncode(const std::string& data);

/**
 * @brief Assembles the complete HTTP GET request URL for the tracker.
 *
 * @param announceUrl The base "announce" URL from the torrent file.
 * @param infoHash The 20-byte binary info_hash.
 * @param peerId The 20-byte binary peer_id.
 * @param port The port we are listening on.
 * @param uploaded The total bytes uploaded.
 * @param downloaded The total bytes downloaded.
 * @param left The total bytes left to download.
 * @param compact Option to have respone be compact
 * @return A complete, URL-encoded string for the tracker request.
 */
std::string buildTrackerUrl(
    const std::string& announceUrl,
    const std::vector<unsigned char>& infoHash,
    const std::string& peerId,
    unsigned short port,
    long long uploaded,
    long long downloaded,
    long long left,
    int compact
);

/**
 * @brief Sends an HTTP GET request to the given URL using libcurl.
 *
 * @param url The complete, URL-encoded tracker request URL.
 * @return The raw response body from the tracker (which is bencoded).
 * @throws std::runtime_error on a cURL error.
 */
std::string sendTrackerRequest(const std::string& url);

/**
 * @brief Parses a compact (binary) peer list from a tracker response.
 *
 * @param peers A string where each peer is 6 bytes (4-byte IP, 2-byte Port).
 * @return A vector of Peer structs.
 * @throws std::runtime_error if the peer string has an invalid length.
 */
std::vector<Peer> parseCompactPeers(const std::string& peers);

#endif // TRACKER_H
