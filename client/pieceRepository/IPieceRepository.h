#ifndef IPIECEREPOSITORY_H
#define IPIECEREPOSITORY_H

#include <vector>
#include <cstdint>
#include <string>

/**
 * @brief Interface for Data Storage and Verification.
 * 1. What pieces client has on disk.
 * 2. Reading/Writing data to disk storage. 
 * 3. Verifying data integrity against torrent hashes.
 */
class IPieceRepository {
public:
  virtual ~IPieceRepository() = default;

  /**
   * @brief Initializes the repository storage.
   * Checks for existing files, verifies pieces if they exist, and populates the initial bitfield.
   * @param downloadPath The directory path where files should be stored.
   */
  virtual void initialize(const std::string& downloadPath) = 0;

  /**
   * @brief Retrieves the current local bitfield.
   * @returns std::vector<uint8_t> A copy of the bitfield.
   */
  virtual std::vector<uint8_t> getBitfield() const = 0;

  /**
   * @brief Verifies if the provided data matches the SHA-1 hash for the piece index.
   * @param index The zero-based index of the piece.
   * @param data The complete data buffer for the piece.
   * @returns true if the hashed data matches the expected hash, false if it fails
   */
  virtual bool verifyHash(size_t index, const std::vector<uint8_t>& data) = 0;

  /**
   * @brief Writes data to disk at appropriate offset.
   * @param index The zero-based index of the piece.
   * @param data The verified data buffer.
   * @throws std::runtime_error If I/O operations fail.
   */
  virtual void savePiece(size_t index, const std::vector<uint8_t>& data) = 0;

  /**
   * @brief Reads and returns a specific block of data from storage
   * @param index The zero-based index of the piece.
   * @param begin The byte offset within the piece.
   * @param length The length of data to read.
   * @return std::vector<uint8_t> The requested data.
   * @throws std::runtime_error If the piece is not present or bounds are invalid.
   */
  virtual std::vector<uint8_t> readBlock(size_t index, size_t begin, size_t length) = 0;

  /**
   * @brief Gets the standard length of a piece according to torrent data.
   * @returns The length of pieces in bytes.
   */
  virtual size_t getPieceLength() const = 0;

  /**
   * @brief Gets the standard length of the torrent according to torrent data.
   * @returns The length of torrent in bytes.
   */
  virtual size_t getTotalLength() const = 0;
  
  /**
   * @brief Checks if we possess a specific piece.
   * A helper method to avoid fetching the entire bitfield for a single check.
   * @param index The zero-based index of the piece.
   * @return true If the piece is verified and on disk, false otherwise.
   */
  virtual bool havePiece(size_t index) const = 0;
};

#endif // IPIECEREPOSITORY_H