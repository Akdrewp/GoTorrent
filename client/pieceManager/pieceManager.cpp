#include "pieceManager.h"
#include <cmath>
#include <stdexcept>
#include <cstring> // memcmp
#include <openssl/sha.h>
#include <spdlog/spdlog.h>

PieceManager::PieceManager(std::shared_ptr<ITorrentStorage> storage, const TorrentData& torrent)
  : storage_(std::move(storage)), torrent_(torrent) {}

void PieceManager::initialize(const std::string& downloadPath) {
  auto& infoDict = torrent_.mainData.at("info")->get<BencodeDict>();

  // Parse torrent data
  pieceHashes_ = infoDict.at("pieces")->get<std::string>();
  numPieces_ = pieceHashes_.length() / 20; // Each hash is 20 bytes long
  pieceLength_ = infoDict.at("piece length")->get<long long>();
  totalLength_ = getTotalLengthTorrent(infoDict);

  // Initialize storage
  storage_->initialize(torrent_, pieceLength_, downloadPath);

  // Initialize bitfield
  // Size = ceil(numPieces / 8)
  size_t bitfieldSize = (numPieces_ + 7) / 8;
  myBitfield_.resize(bitfieldSize, 0);

  // TODO: Way to find what piece already exists and populate bitfield
  
  spdlog::info("--- PIECE MANAGER INITIALIZED ---");
  spdlog::info("Pieces: {} | Length: {}", numPieces_, pieceLength_);
}

bool PieceManager::hasPiece(size_t index) const {
  if (index >= numPieces_) return false;
  
  size_t byteIndex = index / 8;
  uint8_t bitIndex = 7 - (index % 8);

  if (byteIndex >= myBitfield_.size()) return false;
  return (myBitfield_[byteIndex] & (1 << bitIndex)) != 0;
}

void PieceManager::updateBitfield(size_t pieceIndex) {
  size_t byteIndex = pieceIndex / 8;
  uint8_t bitIndex = 7 - (pieceIndex % 8);

  std::lock_guard<std::mutex> lock(mutex_);
  if (byteIndex < myBitfield_.size()) {
    myBitfield_[byteIndex] |= (1 << bitIndex);
  }
}

const char* PieceManager::getHashForPiece(size_t index) const {
  if (index >= numPieces_) return nullptr;
  return pieceHashes_.data() + (index * 20);
}

bool PieceManager::verifyHash(size_t pieceIndex, const std::vector<uint8_t>& data) {
  
  // Calculate hash
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(data.data(), data.size(), hash);

  const char* expectedHash = getHashForPiece(pieceIndex);
  if (!expectedHash) return false;

  // Check calculated hash against 
  return std::memcmp(hash, expectedHash, 20) == 0;
}

bool PieceManager::savePiece(size_t pieceIndex, const std::vector<uint8_t>& data) {
  if (!verifyHash(pieceIndex, data)) {
    spdlog::warn("Piece {} Hash Check Failed!", pieceIndex);
    return false;
  }

  // Write to disk
  try {
    storage_->writePiece(pieceIndex, data);
  } catch (const std::exception& e) {
    spdlog::error("Disk Error writing piece {}: {}", pieceIndex, e.what());
    return false;
  }

  // Update State
  updateBitfield(pieceIndex);
  return true;
}

std::vector<uint8_t> PieceManager::readBlock(size_t pieceIndex, size_t begin, size_t length) {
  if (pieceIndex >= numPieces_) throw std::runtime_error("Invalid piece index");
  
  // Optional: Check if we actually have it before reading?
  if (!hasPiece(pieceIndex)) {
    // In a real client, we might disconnect the peer if they ask for something we don't have
    throw std::runtime_error("Peer requested piece we don't have.");
  }

  return storage_->readBlock(pieceIndex, begin, length);
}