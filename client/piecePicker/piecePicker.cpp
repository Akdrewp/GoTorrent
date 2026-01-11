#include "piecePicker.h"
#include <algorithm>
#include <limits>
#include <spdlog/spdlog.h>

PiecePicker::PiecePicker(size_t numPiecesInTorrent) : numPiecesInTorrent_(numPiecesInTorrent) {
  pieceAvailability_.resize(numPiecesInTorrent_, 0);
}

/**
 * @brief Loops through all the pieces in the torent and
 * adds 1 to pieceAvailability if the bitfield contains a piece
 * 
 * @param bitfield Bitfield of peer to add to availability
 */
void PiecePicker::processBitfield(const std::vector<uint8_t>& bitfield) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < numPiecesInTorrent_; ++i) {
    if (hasPiece(bitfield, i)) {
      pieceAvailability_[i]++;
    }
  }
}

/**
 * @brief Loops through all the pieces in the torent and
 * subtracts 1 to pieceAvailability if the bitfield contains a piece
 * 
 * @param bitfield Bitfield of peer to subtract from availability
 */
void PiecePicker::processPeerDisconnect(const std::vector<uint8_t>& bitfield) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < numPiecesInTorrent_; ++i) {
    if (hasPiece(bitfield, i) && pieceAvailability_[i] > 0) {
      pieceAvailability_[i]--;
    }
  }
}

/**
 * @brief Adds 1 to pieceAvailability for piece at index
 * @param index Index of piece recieved from Have message
 */
void PiecePicker::processHave(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index < numPiecesInTorrent_) {
    pieceAvailability_[index]++;
  }
}

/**
 * @brief Selects the optimal piece to download next from a specific peer
 * using the Rarest-First algorithm.
 * 1. Iterates through every piece index in the torrent.
 * 2. Filters out pieces client already possesses (in myBitfield).
 * 3. Filters out pieces currently "In-Flight" (being downloaded by others).
 * 4. Checks if the target peer actually has the piece.
 * 5. Among the valid candidates, identifies the group with the lowest 
 * availability count (rarest in the swarm).
 * 6. Selects a piece from that rarest group (currently the first one found).
 * @param peerBitfield The bitfield of the peer we are requesting from.
 * @param myBitfield Our current bitfield (what we already have).
 * @return std::optional<size_t> The index of the chosen piece, or nullopt if no suitable piece is found.
 */
std::optional<size_t> PiecePicker::pickPiece(
  const std::vector<uint8_t>& peerBitfield, 
  const std::vector<uint8_t>& myBitfield
) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<size_t> candidates;
  size_t minAvailability = std::numeric_limits<size_t>::max();

  for (size_t i = 0; i < numPiecesInTorrent_; ++i) {
    // 1. Check if we already have it
    if (hasPiece(myBitfield, i)) continue;

    // 2. Check if in-flight
    if (inFlightPieces_.count(i) > 0) continue;

    // 3. Check if peer has it
    if (hasPiece(peerBitfield, i)) {
      size_t rarity = pieceAvailability_[i];
      
      if (rarity < minAvailability) {
        // Found a new rarest level, clear previous candidates
        minAvailability = rarity;
        candidates.clear();
        candidates.push_back(i);
      } else if (rarity == minAvailability) {
        // Same rarity, add to candidates
        candidates.push_back(i);
      }
    }
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  // Pick a random piece from the rarest candidates to distribute load
  // For simplicity, we just pick the first one for now
  size_t selected = candidates[0];
  
  inFlightPieces_.insert(selected);
  return selected;
}

/**
 * @brief Removes the piece index from the `inFlightPieces_` set.
 * @param index The index of the completed piece.
 */
void PiecePicker::onPiecePassed(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  inFlightPieces_.erase(index);
}

/**
 * @brief Removes the piece index from the `inFlightPieces_` set.
 * @param index The index of the completed piece.
 */
void PiecePicker::onPieceFailed(size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  inFlightPieces_.erase(index);
}

/**
 * @brief Helper function to check if a specific bit is set in a bitfield.
 * Calculates the byte index (index / 8) and the bit offset within that byte
 * (7 - (index % 8)).
 * It then uses a bitwise AND to check if that specific bit is 1.
 * 
 * @param bitfield The vector of bytes representing the bitfield.
 * @param index The zero-based index of the piece to check.
 * @return true if the bit is set (1), false otherwise.
 */
bool PiecePicker::hasPiece(const std::vector<uint8_t>& bitfield, size_t index) const {
  size_t byteIndex = index / 8;
  uint8_t bitIndex = 7 - (index % 8);
  if (byteIndex >= bitfield.size()) return false;
  return (bitfield[byteIndex] & (1 << bitIndex)) != 0;
}