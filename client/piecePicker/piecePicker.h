#ifndef PIECE_PICKER_H
#define PIECE_PICKER_H

#include "IPiecePicker.h"
#include <set>
#include <mutex>
#include <vector>

class PiecePicker : public IPiecePicker {
public:
    PiecePicker(size_t numPieces);

    std::optional<size_t> pickPiece(
        const std::vector<uint8_t>& peerBitfield, 
        const std::vector<uint8_t>& myBitfield
    ) override;

    void onPiecePassed(size_t index) override;
    void onPieceFailed(size_t index) override;

    void processBitfield(const std::vector<uint8_t>& bitfield) override;
    void processPeerDisconnect(const std::vector<uint8_t>& bitfield) override;
    void processHave(size_t index) override;

private:
    size_t numPiecesInTorrent_;
    
    // Tracks pieces currently being downloaded by ANY peer.
    std::set<size_t> inFlightPieces_;
    
    // Tracks how many peers have each piece
    std::vector<size_t> pieceAvailability_;
    
    std::mutex mutex_;
    
    /**
     * @brief Checks whether a given bitfield has a certain piece
     * @param bitfield Bitfield to check
     * @param index Index of piece to check existence of
     * @returns true if the bitfield contains the piece index, false otherwise
     */
    bool hasPiece(const std::vector<uint8_t>& bitfield, size_t index) const;
};

#endif // PIECE_PICKER_H