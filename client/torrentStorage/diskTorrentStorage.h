#ifndef DISK_TORRENT_STORAGE_H
#define DISK_TORRENT_STORAGE_H

#include "ITorrentStorage.h"
#include <fstream>
#include <string>
#include <filesystem>
#include <list>
#include <unordered_map>

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

  /**
   * @brief Reads a specific block from the file.
   * Calculates the global file offset based on piece index and begin offset
   * then reads the bytes from there
   * 
   * @param pieceIndex The index of the piece.
   * @param begin The byte offset within that piece.
   * @param length The number of bytes to read.
   * @return The data read from disk.
   */
  std::vector<uint8_t> readBlock(size_t pieceIndex, size_t begin, size_t length) override;


  // Struct to hold the data of each file for read and write
  struct FileEntry {
    std::filesystem::path path;
    size_t length;
    size_t globalOffset;
  };

  // Pair: <File Path String, File Stream>
  using FilePoolList = std::list<std::pair<std::string, std::unique_ptr<std::fstream>>>;

private:

  /**
   * @brief Helper to physically create directories and empty files on disk
   * during initialization.
   */
  void createFileStructure();

  /**
   * @brief Checks if a file exists and throws an error if it does.
   * Used during initialization to prevent accidental overwrites until resume logic is added.
   * @param path Path to check.
   */
  void handleExistingFile(const std::filesystem::path& path);

  /**
   * @brief Reads raw bytes from the file stream at a specific global offset.
   */
  std::vector<uint8_t> readBytes(long long offset, size_t length);

  // Maximum number of files in pool
  static const size_t MAX_OPEN_FILES = 64;

  /**
   * @brief Retrieves a pointer to an open file stream for the given path.
   * If the file is already open, returns it
   * If not, opens it and closes a file if max files has been reached
   * @param path The full path to the file.
   * @return Pointer to the open fstream (owned by the pool).
   */
  std::fstream* getFileStream(const std::filesystem::path& path);

  FilePoolList filePool_;

  std::unordered_map<std::string, FilePoolList::iterator> openFilesMap_;

  std::vector<FileEntry> files_;
  std::string downloadDirectory_;
  long long pieceLength_ = 0;
};

#endif // DISK_TORRENT_STORAGE_H