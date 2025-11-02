#include "tracker.h"

/**
 * @brief URL-encodes a string of raw bytes.
 */
std::string urlEncode(const std::vector<unsigned char>& data) {
  std::stringstream ss;
  // Set output to hex and fill with '0'
  ss << std::hex << std::setfill('0');
  
  for (unsigned char c : data) {
    // Check if char needs to be escaped
    // These are the characters that do not need to be encoded
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      ss << c;
    } else {
      // It's a reserved char. Encode it in %XX format
      ss << '%' << std::setw(2) << static_cast<int>(c);
    }
  }
  
  return ss.str();
}

/**
 * @brief Overload for URL-encoding a standard string.
 */
std::string urlEncode(const std::string& data) {
  // Convert std::string to std::vector<unsigned char> and call the other function
  std::vector<unsigned char> vec(data.begin(), data.end());
  return urlEncode(vec);
}

/**
 * @brief Assembles the complete HTTP GET request URL for the tracker.
 * 
 * 
 * compact=1 set by default as it is most common to send this
 * style for trackers.
 * https://www.bittorrent.org/beps/bep_0023.html
 *
 * @param announceUrl The base "announce" URL from the torrent file.
 * @param infoHash The 20-byte binary info_hash.
 * @param peerId The 20-byte binary peer_id.
 * @param port The port we are listening on.
 * @param uploaded The total bytes uploaded.
 * @param downloaded The total bytes downloaded.
 * @param left The total bytes left to download.
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
    int compact=1 // Default to 1
) {
  std::stringstream ss;

  // Start with the base URL
  ss << announceUrl;
  
  // Check if the URL already has a '?'
  if (announceUrl.find('?') == std::string::npos) {
    ss << '?';
  } else {
    ss << '&';
  }

  // Add all the parameters
  // The binary data *must* be encoded
  ss << "info_hash=" << urlEncode(infoHash);
  ss << "&peer_id="  << urlEncode(peerId);
  
  // The numeric data is just converted to a string
  ss << "&port="       << port;
  ss << "&uploaded="   << uploaded;
  ss << "&downloaded=" << downloaded;
  ss << "&left="       << left;
  ss << "&compact=" << compact;
  
  // Add the event=started for the first request
  ss << "&event=started";

  return ss.str();
}
