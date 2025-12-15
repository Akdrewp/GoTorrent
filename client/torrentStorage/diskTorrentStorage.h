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
    void initialize(const TorrentData& torrent, long long pieceLength) override;
    void writePiece(size_t pieceIndex, const std::vector<uint8_t>& data) override;

private:
    std::string outputFilename_;
    std::fstream outputFile_;
    long long pieceLength_ = 0;
};

#endif // DISK_TORRENT_STORAGE_H