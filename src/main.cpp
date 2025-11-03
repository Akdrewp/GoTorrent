#include "torrent.h"  // Torrent logic header
#include "tracker.h"  // Tracker logic header
#include <iostream>   // For std::cout, std::cerr
#include <iomanip>    // For std::setw, std::hex
#include <variant>    // For std::visit
#include <stdexcept>  // For std::runtime_error
#include <random>     // For std::random_device

// --- Printing Functions (for debugging) ---

// Forward declaration for recursion
void printBencodeValue(const BencodeValue& bv, int indent = 0);

/**
 * @brief Helper struct for using std::visit to print BencodeValue
 */
struct BencodePrinter {
  int indent;
  void operator()(long long val) const {
    std::cout << val;
  }
  void operator()(const std::string& val) const {
    std::cout << "\"";
    for(unsigned char c : val) {
      if (std::isprint(c) && c != '\\' && c != '\"') {
        std::cout << c;
      } else {
        std::cout << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::dec;
      }
    }
    std::cout << "\"";
  }
  void operator()(const BencodeList& list) const {
    std::cout << "[\n";
    for (const auto& item_ptr : list) {
      std::cout << std::string(indent + 2, ' ');
      printBencodeValue(*item_ptr, indent + 2); // Recursive call
      std::cout << ",\n";
    }
    std::cout << std::string(indent, ' ') << "]";
  }
  void operator()(const BencodeDict& dict) const {
    std::cout << "{\n";
    for (const auto& [key, val_ptr] : dict) {
      std::cout << std::string(indent + 2, ' ') << "\"" << key << "\": ";
      if (key == "pieces") {
        std::cout << "\"(... pieces data redacted ...)\"";
      } else {
        printBencodeValue(*val_ptr, indent + 2); // Recursive call
      }
      std::cout << ",\n";
    }
    std::cout << std::string(indent, ' ') << "}";
  }
};

/**
 * @brief Pretty-prints a parsed BencodeValue.
 */
void printBencodeValue(const BencodeValue& bv, int indent) {
  std::visit(BencodePrinter{indent}, bv.value);
}

/**
 * @brief Generates a 20-byte, BEP 20 compliant peer_id.
 * * Format: "-GT0001-<12 random digits>"
 * 
 * BitTorrent specification has a page on "Peer ID Conventions"
 * https://www.bittorrent.org/beps/bep_0020.html
 * 
 * Using Mainline style:
 * - Start with dash
 * 
 * GT Two characters to identify client implementation (GoTorrent)
 * 
 * 0001 number representing the version number
 * 
 * - End main with dash
 * 
 * <12 random digits> a string of 12 bytes each a digit
 * Using digits to avoid escaping characters
 * 
 * @return A 20-byte string.
 */
std::string generatePeerId() {
  // Set client prefix
  // "GoTorrent" v0.0.1
  const std::string clientPrefix = "-GT0001-";

  // Set up the random part
  std::string peerId = clientPrefix;
  peerId.reserve(20); // Reserve 20 bytes total

  // Use C++11 <random> to generate 12 random digits
  std::random_device rd;  // Seed
  std::mt19937 gen(rd()); // Mersenne Twister engine
  std::uniform_int_distribution<> dis(0, 9); // Distribution for 0-9

  for (int i = 0; i < 12; ++i) {
    peerId += std::to_string(dis(gen));
  }
  
  return peerId;
}


int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_torrent_file>" << std::endl;
    return 1;
  }

  std::string torrentFilePath = argv[1];

  try {
    // Parse torrent file
    TorrentData torrent = parseTorrentFile(torrentFilePath);

    // Generate a peer_id
    std::string peerId = generatePeerId();
    
    // Get a port to listen to
    // For now just use 6881
    long long port = 6881;

    // Get announce URL
    auto& announceVal = torrent.mainData.at("announce")->value;
    std::string announceUrl;
    if (auto* url = std::get_if<std::string>(&announceVal)) {
      announceUrl = *url;
    } else {
      throw std::runtime_error("Announce URL is not a string.");
    }

    // Get total length (for initial "left")
    auto& infoVal = torrent.mainData.at("info")->value;
    long long totalLength = 0;
    if (auto* infoDict = std::get_if<BencodeDict>(&infoVal)) {
      // It's a dictionary! Pass it to our helper function.
      // infoDict is a BencodeDict*, so we dereference it with *
      totalLength = getTotalLength(*infoDict);
    } else {
      throw std::runtime_error("Torrent 'info' field is not a dictionary.");
    }

    // Build the Tracker GET URL
    std::string trackerUrl = buildTrackerUrl(
      announceUrl,
      torrent.infoHash,
      peerId,
      port, // port
      0,    // uploaded
      0,    // downloaded
      totalLength,
      1     //compact
    );


    // Print the results
    std::cout << "\n--- PREPARING TRACKER REQUEST ---" << std::endl;
    std::cout << "Peer ID: " << peerId << std::endl;
    std::cout << "Announce URL: " << announceUrl << std::endl;
    std::cout << "Total Length (left): " << totalLength << std::endl;

    // Print the hash
    std::cout << "--- INFO HASH ---" << std::endl;
    std::cout << std::hex << std::setfill('0');
    for (unsigned char c : torrent.infoHash) {
      std::cout << std::setw(2) << static_cast<int>(c);
    }
    std::cout << std::dec << std::endl;

    // Print the final URL
    std::cout << "\n--- COMPLETE TRACKER URL ---" << std::endl;
    std::cout << trackerUrl << std::endl;

    std::cout << "\n--- SENDING REQUEST TO TRACKER ---" << std::endl;
    std::string trackerResponse = sendTrackerRequest(trackerUrl);

    std::cout << "Tracker raw response size: " << trackerResponse.size() << " bytes" << std::endl;

    // Parse the tracker's response
    // Should be in bencode
    std::cout << "\n--- PARSING TRACKER RESPONSE ---" << std::endl;
    size_t index = 0;
    // Convert string to vector<char> for our parser
    std::vector<char> responseBytes(trackerResponse.begin(), trackerResponse.end());
    BencodeValue parsedResponse = parseBencodedValue(responseBytes, index);
    
    // Print the parsed response
    printBencodeValue(parsedResponse);
    std::cout << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
