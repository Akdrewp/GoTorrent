#ifndef DISK_TORRENT_STORAGE_H
#define DISK_TORRENT_STORAGE_H

#include "ITorrentStorage.h"
#include <fstream>
#include <string>

/**
 * @brief Implementation of storage that writes directly to the local disk.
 * Currently supports single-file torrents.
 */
class DiskTorrentStorage : public ITorrentStorage {
public:
  /**
   * @brief Configures the storage based on the torrent metadata.
   * Determines the file structure and calls
   * initializeSingleFile() for single file torrents
   * @param torrent The parsed torrent data containing file info.
   * @param pieceLength The length of a standard piece in bytes.
   * @param downloadDirectory The base directory where files will be saved.
   */
  void initialize(const TorrentData& torrent, long long pieceLength, const std::string& downloadDirectory) override;

  /**
   * @brief Writes a verified piece to the open file stream.
   * Calculates the correct byte offset based on the piece index.
   * 
   * @param pieceIndex Index of the completed piece to be written
   * @param data Data of the completed piece
   */
  void writePiece(size_t pieceIndex, const std::vector<uint8_t>& data) override;

private:

  /**
   * @brief Initializes storage for a Single-File torrent.
   * Creates the necessary directory and opens/creates the output file 
   * based on the stored outputFilePath_ and std::string downloadDirectory_
   */
  void initializeSingleFile();

  std::string downloadDirectory_;
  std::string outputFilename_;
  std::fstream outputFile_;
  long long pieceLength_ = 0;
};

#endif // DISK_TORRENT_STORAGE_H