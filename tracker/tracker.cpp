#include "tracker.h"
#include <curl/curl.h> // For curl

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
 * @brief cURL callback function to write response data into a std::string.
 *
 * Provided by libcurl
 *
 * @param contents Pointer to the data chunk received.
 * @param size Size of each data element (always 1 byte).
 * @param nmemb Number of data elements.
 * @param userp Pointer to our std::string.
 * @return The total number of bytes handled.
 */
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  // userp is the std::string* we passed in
  // Append the new data to it.
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

/**
 * @brief Sends an HTTP GET request to the given URL using libcurl.
 */
std::string sendTrackerRequest(const std::string& url) {

  CURL *curl;
  CURLcode res;
  std::string responseBuffer; // This string will hold our response

  curl = curl_easy_init();
  if (curl) {
      // Set the URL
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

      // Set the "write function" (our callback)
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);

      // Set the "write data" (the string to write to)
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);

      // Set a timeout 10 seconds
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

      // Perform the request
      // Since no other options are selected
      // automatically sends GET request
      res = curl_easy_perform(curl);

      // Clean up
      curl_easy_cleanup(curl);

      if (res != CURLE_OK) {
          // Request failed
          throw std::runtime_error("curl_easy_perform() failed: " + std::string(curl_easy_strerror(res)));
      }
  } else {
      throw std::runtime_error("curl_easy_init() failed.");
  }

  return responseBuffer;
}

/**
 * @brief Assembles the complete HTTP GET request URL for the tracker.
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
