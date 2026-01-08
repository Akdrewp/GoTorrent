#ifndef PIECE_MANAGER_H
#define PIECE_MANAGER_H

#include "ITorrentStorage.h"
#include "torrent.h"
#include "bencode.h"
#include <vector>
#include <string>
#include <memory>
#include <mutex>

/**
 * @brief Manages the physical data, bitfield, and hash verification of the torrent pieces
 */
class PieceManager {
public:
    PieceManager(std::shared_ptr<ITorrentStorage> storage, const TorrentData& torrent);

    /**
     * @brief Initializes storage and parses torrent info for hashes and piece length
     * @param downloadPath Path to save files
     */
    void initialize(const std::string& downloadPath);

    /**
     * @brief Writes a downloaded piece to disk if it passes hash check
     * @return true if hash matched and write succeeded, false otherwise
     */
    bool savePiece(size_t pieceIndex, const std::vector<uint8_t>& data);

    /**
     * @brief Reads a block of data for uploading to a peer
     */
    std::vector<uint8_t> readBlock(size_t pieceIndex, size_t begin, size_t length);

    /**
     * @brief Checks if client have a specific piece
     */
    bool hasPiece(size_t index) const;

    // --- Getters ---
    size_t getNumPieces() const { return numPieces_; }
    long long getPieceLength() const { return pieceLength_; }
    long long getTotalLength() const { return totalLength_; }
    const std::string& getPieceHashes() const { return pieceHashes_; }
    const std::vector<uint8_t>& getBitfield() const { return myBitfield_; }

    // Returns a raw pointer to the hash for a specific index
    const char* getHashForPiece(size_t index) const;

private:
    std::shared_ptr<ITorrentStorage> storage_;
    const TorrentData& torrent_; // Reference to torrent data held by Session

    // Torrent Info
    std::string pieceHashes_; // The concatenated hash string
    size_t numPieces_ = 0;
    long long pieceLength_ = 0;
    long long totalLength_ = 0;

    // State
    std::vector<uint8_t> myBitfield_;
    mutable std::mutex mutex_; // Protects bitfield access

    // Helpers
    void updateBitfield(size_t pieceIndex);
    bool verifyHash(size_t pieceIndex, const std::vector<uint8_t>& data);
};

#endif // PIECE_MANAGER_H