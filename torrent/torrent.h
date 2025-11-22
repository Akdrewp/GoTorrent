#ifndef TORRENT_H
#define TORRENT_H

#include "bencode.h" // For BencodeDict
#include <iostream>  // For cerr
#include <fstream>
#include <vector>    // For vector
#include <string>    // For string
#include <iomanip>   // For setfill

#include <openssl/sha.h> // For SHA-1

/**
 * @brief Holds the key data parsed from a .torrent file.
 */
struct TorrentData {
    BencodeDict mainData;               // The full parsed dictionary
    std::vector<unsigned char> infoHash; // The 20-byte SHA-1 info_hash
};

/**
 * @brief Reads and parses a .torrent file.
 * @param torrentFilePath The path to the .torrent file.
 * @return A TorrentData struct containing the parsed data and info_hash.
 * @throws std::runtime_error on file or parsing errors.
 */
TorrentData parseTorrentFile(const std::string& torrentFilePath);

/**
 * @brief Gets the total length from a Bencode info dictionary
 * Single file torrents have a "length" field to read
 * 
 * Multi file torrents have a "files" key each with their own
 * "length" field which must be totalled up
 * @param infoDict The torrent info dictionary
 * @return The total length of the file(s) in the info dictionary
 * @throws std::runtime_error on incorrect format
 */
long long getTotalLengthTorrent(const BencodeDict& infoDict);

#endif // TORRENT_H
