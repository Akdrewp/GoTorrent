#include "torrent.h"
#include <iterator> // For std::istreambuf_iterator

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
    // Throw an exception instead of returning empty
    throw std::runtime_error("Failed to open file: " + filepath);
  }

  // Get file size
  std::streamsize fileSize = file.tellg();

  // Read from beginning
  file.seekg(0, std::ios::beg);

  // Create byte vector size of file
  std::vector<char> fileContents;
  fileContents.reserve(fileSize);

  // Read file from beginning to end
  fileContents.insert(
    fileContents.begin(),
    std::istreambuf_iterator<char>(file),
    std::istreambuf_iterator<char>()
  );
  file.close();

  return fileContents;
}

/**
 * @brief Reads and parses a .torrent file.
 * @param torrentFilePath The path to the .torrent file.
 * @return A TorrentData struct containing the parsed data and info_hash.
 * @throws std::runtime_error on file or parsing errors.
 */
TorrentData parseTorrentFile(const std::string& torrentFilePath) {

  std::vector<char> fileBytes;

  fileBytes = readTorrentFile(torrentFilePath);
  
  if (fileBytes.empty()) {
    throw std::runtime_error("Failed to read file (file was empty).");
  }
  
  std::cout << "Success! Read " << fileBytes.size() << " bytes from " << torrentFilePath << std::endl;

  TorrentData torrent;
  size_t index = 0;

  // The whole file must be a dictionary
  if (index >= fileBytes.size() || fileBytes[index] != 'd') {
    throw std::runtime_error("Torrent file is not a dictionary.");
  }
  // Skip 'd'
  index++;
  
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

      // Store the hash
      torrent.infoHash = std::move(hash);

      // Store the parsed info dictionary
      torrent.mainData.emplace(std::move(key), std::make_unique<BencodeValue>(std::move(info_bv)));

    } else {
      // It's a different key (e.g., "announce")
      // Just parse the value and store it
      BencodeValue val_bv = parseBencodedValue(fileBytes, index);
      torrent.mainData.emplace(std::move(key), std::make_unique<BencodeValue>(std::move(val_bv)));
    }
  }

  if (index == fileBytes.size()) {
    throw std::runtime_error("Main dictionary not terminated by 'e'.");
  }
  index++; // Skip the final 'e'

  // Check if we found the info hash
  if (torrent.infoHash.empty()) {
    throw std::runtime_error("Parsing error: 'info' dictionary not found in torrent file.");
  }

  return torrent;
}


/**
 * @brief Gets the total length from a Bencode info dictionary
 * Single file torrents have a "length" field to read
 * 
 * Multi file torrents have a "files" key each with their own
 * "length" field which must be totalled up
 * @param infoDict The torrent info dictionary
 * @return The total length of the file(s) in the info dictionary
 * @throws std::runtime_error on incorrect format
 */
long long getTotalLength(const BencodeDict& infoDict) {

  /**
   * std::get_if<<BencodeValue Type>>(&(<BencodeDict iterator>->second->value))
   * <BencodeDict iterator> is the iterator for our map
   * 
   * <BencodeDict iterator>->second gets the second value
   * which is the "value" part.
   * <BencodeDict iterator>->first would be the key for the map
   * 
   * <BencodeDict iterator>->second->value
   * Since <BencodeDict iterator>->second holds a pointer to a map
   * access it via pointer.
   * 
   * std::get_if<<BencodeValue Type>>(&(<BencodeDict iterator>->second->value))
   * Gets the BencodeValue Type from the "value"
   *
   */
  // Case 1: Single-file torrent. Check for "length" key.
  auto lengthIt = infoDict.find("length");
  if (lengthIt != infoDict.end()) {
    // Found "length", get the value
    // The value is a unique_ptr<BencodeValue>, we dereference it
    // then access its .value (the variant), then get the long long
    if (auto* val = std::get_if<long long>(&(lengthIt->second->value))) {
        return *val;
    } else {
        throw std::runtime_error("Invalid torrent: 'length' is not an integer.");
    }
  }

  // Case 2: Multi-file torrent. Check for "files" key.
  auto filesIt = infoDict.find("files");
  if (filesIt != infoDict.end()) {
    long long totalSize = 0;
    
    // Get the list of files
    if (auto* fileList = std::get_if<BencodeList>(&(filesIt->second->value))) {
      // Loop through each file in the list
      for (const auto& file_bv_ptr : *fileList) {
        // Each item in the list is a dictionary
        if (auto* fileDict = std::get_if<BencodeDict>(&(file_bv_ptr->value))) {
          
          // Find the "length" key in this file's dictionary
          auto fileLengthIt = fileDict->find("length");
          if (fileLengthIt != fileDict->end()) {
            if (auto* fileLen = std::get_if<long long>(&(fileLengthIt->second->value))) {
              totalSize += *fileLen;
            }
          }
        }
      }
      return totalSize;
    } else {
      throw std::runtime_error("Invalid torrent: 'files' is not a list.");
    }
  }

  // Case 3: Neither key was found.
  throw std::runtime_error("Invalid 'info' dictionary: missing 'length' or 'files'.");
}