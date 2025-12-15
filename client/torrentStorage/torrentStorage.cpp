#include "diskTorrentStorage.h"
#include "bencode.h"
#include <iostream>
#include <stdexcept>
#include <variant>

void DiskTorrentStorage::initialize(const TorrentData& torrent, long long pieceLength) {
  auto& infoVal = torrent.mainData.at("info")->value;
  auto* infoDict = std::get_if<BencodeDict>(&infoVal);
  if (!infoDict) throw std::runtime_error("Storage: Invalid info dictionary.");

  pieceLength_ = pieceLength;

  // Open File (Single File Mode)
  if (infoDict->count("name")) {
    if (auto* name = std::get_if<std::string>(&(infoDict->at("name")->value))) {
      outputFilename_ = *name;
      std::cout << "Storage: Output file is " << outputFilename_ << std::endl;

      // Open for Read/Write binary
      outputFile_.open(outputFilename_, std::ios::binary | std::ios::in | std::ios::out);
      if (!outputFile_.is_open()) {
        std::cout << "Storage: File not found, creating..." << std::endl;
        outputFile_.open(outputFilename_, std::ios::binary | std::ios::out);
      }
      
      if (!outputFile_.is_open()) {
        throw std::runtime_error("Storage: Failed to open output file: " + outputFilename_);
      }
    } else {
      throw std::runtime_error("Storage: Torrent 'name' is not a string.");
    }
  } else {
    throw std::runtime_error("Storage: Torrent has no 'name' (multi-file not supported).");
  }
}

void DiskTorrentStorage::writePiece(size_t pieceIndex, const std::vector<uint8_t>& data) {
if (!outputFile_.is_open()) {
    std::cerr << "--- STORAGE: ERROR! Output file is not open. Cannot write piece " << pieceIndex << " ---" << std::endl;
    return;
  }

  // Calculate the byte offset in the file
  long long offset = static_cast<long long>(pieceIndex) * pieceLength_;
  std::cout << "--- STORAGE: Writing piece " << pieceIndex << " to disk at offset " << offset << " (" << data.size() << " bytes) ---" << std::endl;

  // Seek to the correct position
  outputFile_.seekp(offset, std::ios::beg);
  if (outputFile_.fail()) {
    std::cerr << "--- SESSION: ERROR! Failed to seek to offset " << offset << " ---" << std::endl;
    outputFile_.clear(); // Clear error state
    return;
  }

  // Write the data at position
  outputFile_.write(reinterpret_cast<const char*>(data.data()), data.size());
  if (outputFile_.fail()) {
    std::cerr << "--- SESSION: ERROR! Failed to write data for piece " << pieceIndex << " ---" << std::endl;
    outputFile_.clear(); // Clear error state
    return;
  }

  outputFile_.flush();
  if (outputFile_.fail()) {
    std::cerr << "--- SESSION: ERROR! Failed to flush file stream for piece " << pieceIndex << " ---" << std::endl;
    outputFile_.clear(); // Clear error state
  }
}