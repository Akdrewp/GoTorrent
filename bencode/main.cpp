#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <algorithm> // For std::search, std::find, std::distance
#include "bencode.h"

// SHA-1 hashing
#include <openssl/sha.h>

/**
 * @brief Reads the entire contents of a binary file into a vector.
 * @param filename The path to the file to read.
 * @return A std::vector<char> containing the file's bytes, or an empty
 * vector if the file could not be opened.
 */
std::vector<char> readTorrentFile(const std::string& filepath) {
  // Flags
  // std::ios::binary read as raw bytes
  // std::ios:ate start reading from the end
  std::ifstream file(filepath, std::ios::binary | std::ios::ate);

  if (!file.is_open()) {
  std::cerr << "Error: Could not open file: " << filepath << std::endl;
  return {}; // Return an empty vector on failure
  }

  // Get file size
  std::streamsize fileSize = file.tellg();

  // Read from beginning
  file.seekg(0, std::ios::beg);

  // Create byte vector size of file
  std::vector<char> fileContents;
  fileContents.reserve(fileSize);

  // Read file from beggining to end
  fileContents.insert(
  fileContents.begin(),
  std::istreambuf_iterator<char>(file),
  std::istreambuf_iterator<char>()
  );
  file.close();

  return fileContents;
}

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
    // Print non-printable chars as hex, for safety
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

      // Don't print the 'pieces' value
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

void printBencodeValue(const BencodeValue& bv, int indent) {
  std::visit(BencodePrinter{indent}, bv.value);
}



int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_torrent_file>" << std::endl;
    return 1;
  }

  std::string torrentFilePath = argv[1];
  std::vector<char> fileBytes;

  try {
    fileBytes = readTorrentFile(torrentFilePath);
  } catch (const std::exception& e) {
    std::cerr << "Error reading file: " << e.what() << std::endl;
    return 1;
  }

  if (!fileBytes.empty()) {
  std::cout << "Success! Read " << fileBytes.size() << " bytes from " << torrentFilePath << std::endl;
  
  try {
    size_t index = 0;

    // The whole file must be a dictionary
    if (index >= fileBytes.size() || fileBytes[index] != 'd') {
      throw std::runtime_error("Torrent file is not a dictionary.");
    }
    // Skip 'd'
    index++;

    BencodeDict mainData;


    // Loop until end of mainData dictionary
    while (index < fileBytes.size() && fileBytes[index] != 'e') {

      // Parse current dict key
      BencodeValue key_bv = parseString(fileBytes, index);
      std::string key = std::get<std::string>(key_bv.value);

      if (key == "info") {
        // Get start index
        size_t infoStartIndex = index;

        // Parse info dictionary
        // This moves index to the end of the info dictionary
        BencodeValue info_bv = parseBencodedValue(fileBytes, index);
        size_t infoEndIndex = index;

        // Calculate info hash on info field
        std::vector<char> infoBytes(fileBytes.begin() + infoStartIndex, fileBytes.begin() + infoEndIndex);
        std::vector<unsigned char> hash(SHA_DIGEST_LENGTH);
        SHA1(reinterpret_cast<const unsigned char*>(infoBytes.data()), infoBytes.size(), hash.data());

        // Print the hash
        std::cout << "--- INFO HASH ---" << std::endl;
        std::cout << std::hex << std::setfill('0');
        for (unsigned char c : hash) {
          std::cout << std::setw(2) << static_cast<int>(c);
        }
        std::cout << std::dec << std::endl; // Reset to decimal

        // Store the parsed info dictionary
        mainData.emplace(std::move(key), std::make_unique<BencodeValue>(std::move(info_bv)));

      } else {
        // It's a different key (e.g., "announce")
        // Just parse the value and store it
        BencodeValue val_bv = parseBencodedValue(fileBytes, index);
        mainData.emplace(std::move(key), std::make_unique<BencodeValue>(std::move(val_bv)));
      }
    }

  } catch (const std::exception& e) {
    std::cerr << "Parsing Error: " << e.what() << std::endl;
    return 1;
  }
  
  } else {
    std::cerr << "Failed to read file (file was empty)." << std::endl;
    return 1;
  }

  return 0;
}