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
  auto& infoDict = torrent.mainData.at("info")->get<BencodeDict>();

  pieceLength_ = pieceLength;
  downloadDirectory_ = downloadDirectory;

  // Single file torrent
  if (infoDict.count("name")) {
    outputFilename_ = infoDict.at("name")->get<std::string>();
    initializeSingleFile();
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

// --- readBlock ---

std::vector<uint8_t> DiskTorrentStorage::readBlock(size_t pieceIndex, size_t begin, size_t length) {
  // Calculate global offset
  long long offset = (static_cast<long long>(pieceIndex) * pieceLength_) + begin;
  
  // Read the bytes in block
  return readBytes(offset, length);
}

std::vector<uint8_t> DiskTorrentStorage::readBytes(long long offset, size_t length) {
  if (!outputFile_.is_open()) {
    throw DiskStorageError("Read failed: Output file is not open.");
  }

  // Seek to position
  outputFile_.seekg(offset, std::ios::beg);
  if (outputFile_.fail()) {
    outputFile_.clear();
    throw DiskStorageError("Read failed: Could not seek to offset " + std::to_string(offset));
  }

  std::vector<uint8_t> buffer(length);
  
  // Read bytes
  outputFile_.read(reinterpret_cast<char*>(buffer.data()), length);

  // Check results
  if (outputFile_.fail() || static_cast<size_t>(outputFile_.gcount()) != length) {
    outputFile_.clear();
    throw DiskStorageError("Read failed: Requested " + std::to_string(length) + 
                            " bytes but read " + std::to_string(outputFile_.gcount()) + 
                            " from offset " + std::to_string(offset));
  }

  return buffer;
}