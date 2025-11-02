#ifndef TRACKER_H
#define TRACKER_H

#include <string>
#include <vector>
#include <sstream>   // For std::stringstream
#include <iomanip>   // For std::hex, std::setw
#include <cctype>    // For std::isalnum

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


#endif // TRACKER_H
