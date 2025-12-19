#include "diskTorrentStorage.h"
#include "bencode.h"
#include <iostream>
#include <stdexcept>
#include <variant>
#include <filesystem>

namespace fs = std::filesystem;

// Helper Log functions

static void StorageLog(const std::string& message) {
  std::cout << "Storage: " << message << std::endl;
}

static std::runtime_error DiskStorageError(const std::string& message) {
  return std::runtime_error("Storage Error: " + message);
}

// End

void DiskTorrentStorage::initialize(const TorrentData& torrent, long long pieceLength, const std::string& downloadDirectory) {
  auto& infoVal = torrent.mainData.at("info")->value;
  auto* infoDict = std::get_if<BencodeDict>(&infoVal);
  if (!infoDict) throw DiskStorageError("Invalid info dictionary.");

  pieceLength_ = pieceLength;
  downloadDirectory_ = downloadDirectory;

  // Single file torrent
  if (infoDict->count("name")) {
    if (auto* name = std::get_if<std::string>(&(infoDict->at("name")->value))) {
      outputFilename_ = *name;
      initializeSingleFile();
    } else {
      throw DiskStorageError("Torrent 'name' is not a string.");
    }
  } else { // Multifile torrent
    throw DiskStorageError("Torrent has no 'name' (multi-file not supported).");
  }
}

// initializeSingleFile

/**
 * @brief Helper for initializeSingleFile()
 * 
 * Creates the output file if it doesn't exist and opens it
 * 
 * @param outputFile The file stream
 * @param fullPath Path to file
 */
static void createAndOpenOutputFile(std::fstream& outputFile, const fs::path& fullPath) {
  // Check if file exists. If so, throw error
  // May be handled differently perhaps by asking if user wants to overwrite
  if (fs::exists(fullPath)) {
    throw DiskStorageError("File already exists: " + fullPath.string());
  }

  StorageLog("Creating new file...");

  // Create empty file and close
  outputFile.open(fullPath, std::ios::binary | std::ios::out);
  outputFile.close();

  // Open in Read/Write mode for random access
  outputFile.open(fullPath, std::ios::binary | std::ios::in | std::ios::out);

  if (!outputFile.is_open()) {
    throw DiskStorageError("Failed to open output file: " + fullPath.string());
  }
}

/**
 * @brief Initializes a single file torrent
 * 
 * Creates the file in the specified path
 * 
 * If the path's folder doesn't exist creates it
 * 
 * If the file already exists then keeps it the same
 */
void DiskTorrentStorage::initializeSingleFile() {
  // Setup Directory and Path
  fs::path dirPath(downloadDirectory_);
  
  // Create directory if it doesn't exist
  if (!fs::exists(dirPath)) {
    StorageLog("Creating directory '" + dirPath.string() + "'");
    fs::create_directories(dirPath);
  }

  // Construct full path: downloadDirectory/filename
  fs::path fullPath = dirPath / outputFilename_;
  StorageLog("Output file path is " + fullPath.string());

  createAndOpenOutputFile(outputFile_, fullPath);
}

void DiskTorrentStorage::writePiece(size_t pieceIndex, const std::vector<uint8_t>& data) {
  if (!outputFile_.is_open()) {
    throw DiskStorageError("Output file is not open. Cannot write piece " + std::to_string(pieceIndex));
    return;
  }

  // Calculate the byte offset in the file
  long long offset = static_cast<long long>(pieceIndex) * pieceLength_;
  StorageLog("Writing piece " + std::to_string(pieceIndex) + " to disk at offset " + 
             std::to_string(offset) + " (" + std::to_string(data.size()) + " bytes)");

  // Seek to the correct position
  outputFile_.seekp(offset, std::ios::beg);
  if (outputFile_.fail()) {
    outputFile_.clear(); // Clear error state
    throw DiskStorageError("Failed to seek to offset " + std::to_string(offset));
  }

  // Write the data at position
  outputFile_.write(reinterpret_cast<const char*>(data.data()), data.size());
  if (outputFile_.fail()) {
    outputFile_.clear(); // Clear error state
    throw DiskStorageError("Failed to write data for piece " + std::to_string(pieceIndex));
  }

  outputFile_.flush();
  if (outputFile_.fail()) {
    outputFile_.clear(); // Clear error state
    throw DiskStorageError("Failed to flush file stream for piece " + std::to_string(pieceIndex));
  }
}