#include <iostream>
#include <fstream>
#include <vector>
#include "bencode.h"

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

int main(int argc, char* argv[]) {
  // Should be two arguments
  // argv[0] = ./program_name
  // argv[1] = path_to_torrent_file
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_torrent_file>" << std::endl;
    return 1; // Fail
  }

  std::string torrentFilePath = argv[1];

  std::vector<char> fileBytes = readTorrentFile(torrentFilePath);

  if (!fileBytes.empty()) {
    std::cout << "Success! Read " << fileBytes.size() << " bytes from " << torrentFilePath << std::endl;
    
    try {
        // Create our "cursor" to track the position
        size_t index = 0;

        parseBencodedValue(fileBytes, index);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

  
  } else {
      std::cerr << "Failed to read file." << std::endl;
  }

    return 0;
}