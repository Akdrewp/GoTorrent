#ifndef I_TORRENT_STORAGE_H
#define I_TORRENT_STORAGE_H

#include <vector>
#include <cstddef>
#include <cstdint>
#include "torrent.h" // For TorrentData

/**
 * @brief Abstract interface for Torrent Storage.
 * Allows decoupling the session from the physical file system.
 */
class ITorrentStorage {
public:
    virtual ~ITorrentStorage() = default;

    /**
     * @brief Prepares the storage (opens files, creates directories).
     * @param torrent The parsed torrent data containing file info.
     * @param pieceLength Length of each piece
     */
    virtual void initialize(const TorrentData& torrent, long long pieceLength) = 0;

    /**
     * @brief Writes a verified piece to the storage.
     * @param pieceIndex The index of the piece.
     * @param data The raw data of the piece.
     */
    virtual void writePiece(size_t pieceIndex, const std::vector<uint8_t>& data) = 0;
};

#endif // I_TORRENT_STORAGE_H