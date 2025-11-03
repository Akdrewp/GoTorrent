#ifndef CLIENT_H
#define CLIENT_H

#include <string>

/**
 * @brief Runs the main client logic.
 *
 * This function handles parsing the torrent file, building the tracker
 * request, sending it, and parsing the peer response.
 *
 * @param torrentFilePath The path to the .torrent file.
 * @throws std::runtime_error on any fatal error (file read, parse, network).
 */
void runClient(const std::string& torrentFilePath);

#endif // CLIENT_H
