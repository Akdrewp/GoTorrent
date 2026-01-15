#ifndef PIECE_REPOSITORY_H
#define PIECE_REPOSITORY_H

#include "IPieceRepository.h"
#include "ITorrentStorage.h"
#include "torrent.h" // TorrentData
#include "bencode.h"
#include <mutex>
#include <string>

class PieceRepository : public IPieceRepository {
public:
  PieceRepository(std::shared_ptr<ITorrentStorage> storage, const TorrentData& torrent);

  /**
   * @brief Initializes storage and state variables
   * @param downloadPath absolute download path as a string
   */
  void initialize(const std::string& downloadPath);

  // --- IPieceRepository Overrides ---
  std::vector<uint8_t> getBitfield() const override;
  bool verifyHash(size_t index, const std::vector<uint8_t>& data) override;
  void savePiece(size_t index, const std::vector<uint8_t>& data) override;
  std::vector<uint8_t> readBlock(size_t index, size_t begin, size_t length) override;
  size_t getPieceLength() const override { return pieceLength_; }
  size_t getTotalLength() const override { return totalLength_; }
  bool havePiece(size_t index) const override;
  
  // Additional Getter for Factory/Setup
  size_t getNumPieces() const { return numPieces_; }

private:
  // Dependency
  std::shared_ptr<ITorrentStorage> storage_;
  const TorrentData& torrent_;
  
  // Metadata
  std::string pieceHashes_;
  size_t numPieces_ = 0;
  size_t pieceLength_ = 0;
  size_t totalLength_ = 0;

  // State
  std::vector<uint8_t> myBitfield_;
  mutable std::mutex mutex_;

  /**
   * @brief Marks the piece at index as have in bitfield.
   * @param index The zero-based index of the piece to mark as complete.
   */
  void updateBitfield(size_t index);

  /**
   * @brief Retrieves the expected 20-byte SHA-1 hash for a specific piece.
   * @param index The zero-based index of the piece.
   * @returns const char* Pointer to the 20-byte hash buffer, or nullptr if index is out of bounds.
   */
  const char* getHashForPiece(size_t index) const;
};

#endif // PIECE_REPOSITORY_H