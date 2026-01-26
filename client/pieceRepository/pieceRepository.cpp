#include "pieceRepository.h"
#include <cstring>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

PieceRepository::PieceRepository(std::shared_ptr<ITorrentStorage> storage, const TorrentData& torrent)
  : storage_(std::move(storage)), torrent_(torrent) {}

/**
 * @brief Intializes pieceHashes_, numPieces_, pieceLength_, and
 * storage_ from torrent metaData
 */
void PieceRepository::initialize(const std::string& downloadPath) {
  auto& infoDict = torrent_.mainData.at("info")->get<BencodeDict>();
  
  pieceHashes_ = infoDict.at("pieces")->get<std::string>();
  numPieces_ = pieceHashes_.length() / 20;
  pieceLength_ = infoDict.at("piece length")->get<long long>();

  if (infoDict.count("files")) {
    totalLength_ = 0;
    auto& files = infoDict.at("files")->get<BencodeList>();
    for (auto& file : files) {
      totalLength_ += file->get<BencodeDict>().at("length")->get<long long>();
    }
  } else {
    totalLength_ = infoDict.at("length")->get<long long>();
  }

  storage_->initialize(torrent_, pieceLength_, downloadPath);

  // Resize bitfield
  size_t bitfieldSize = (numPieces_ + 7) / 8;
  myBitfield_.resize(bitfieldSize, 0);

  spdlog::info("[Repo] Initialized. Pieces: {}, Length: {}", numPieces_, totalLength_);
}

std::vector<uint8_t> PieceRepository::getBitfield() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return myBitfield_;
}

bool PieceRepository::havePiece(size_t index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t byteIndex = index / 8;
  if (byteIndex >= myBitfield_.size()) return false;
  return (myBitfield_[byteIndex] & (1 << (7 - (index % 8)))) != 0;
}

const char* PieceRepository::getHashForPiece(size_t index) const {
  if (index >= numPieces_) return nullptr;
  return pieceHashes_.data() + (index * 20);
}

bool PieceRepository::verifyHash(size_t index, const std::vector<uint8_t>& data) {
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(data.data(), data.size(), hash);

  const char* expected = getHashForPiece(index);
  if (!expected) return false;

  return std::memcmp(hash, expected, 20) == 0;
}

void PieceRepository::savePiece(size_t index, const std::vector<uint8_t>& data) {
  storage_->writePiece(index, data);
  updateBitfield(index);
  spdlog::info("[Repo] Piece {} written to disk.", index);
}

void PieceRepository::updateBitfield(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t byteIndex = index / 8;
  if (byteIndex < myBitfield_.size()) {
    myBitfield_[byteIndex] |= (1 << (7 - (index % 8)));
  }
}

std::vector<uint8_t> PieceRepository::readBlock(size_t index, size_t begin, size_t length) {
  if (!havePiece(index)) throw std::runtime_error("Do not have piece");
  return storage_->readBlock(index, begin, length);
}