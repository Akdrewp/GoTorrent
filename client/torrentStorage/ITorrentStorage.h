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
   * @param pieceLength The length of a standard piece in bytes.
   * @param downloadDirectory The base directory where files should be saved.
   */
  virtual void initialize(
    const TorrentData& torrent, 
    long long pieceLength, 
    const std::string& downloadDirectory
  ) = 0;

  /**
   * @brief Writes a verified piece to the storage.
   * @param pieceIndex The index of the piece.
   * @param data The raw data of the piece.
   */
  virtual void writePiece(size_t pieceIndex, const std::vector<uint8_t>& data) = 0;

  /**
   * @brief Reads a block of data from storage
   * @param pieceIndex The index of the piece.
   * @param begin The byte offset within that piece.
   * @param length The number of bytes to read.
   * @return The data read from disk.
   */
  virtual std::vector<uint8_t> readBlock(size_t pieceIndex, size_t begin, size_t length) = 0;
};

#endif // I_TORRENT_STORAGE_H