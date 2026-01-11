#ifndef IPIECEPICKER_H
#define IPIECEPICKER_H

#include <vector>
#include <optional>
#include <cstdint>

/**
 * @brief Strategy interface for selecting which pieces to assign to a peer
 */
class IPiecePicker {
public:
    virtual ~IPiecePicker() = default;

    /**
     * @brief Assigns a piece to a peer
     * @param peerBitfield The pieces the remote peer has
     * @param myBitfield The pieces we already have (or have verified).
     * @return std::optional<size_t> Index of the piece to download, or nullopt.
     */
    virtual std::optional<size_t> pickPiece(
        const std::vector<uint8_t>& peerBitfield, 
        const std::vector<uint8_t>& myBitfield
    ) = 0;

    /**
     * @brief Called this when a piece is successfully verified and saved.
     * Piece will be marked as completed
     * @param index Index of the verified and saved piece
     */
    virtual void onPiecePassed(size_t index) = 0;

    /**
     * @brief Called when a piece fails hash check or peer disconnects.
     * Piece will be marked as available
     * @param index Index of the verified and saved piece
     */
    virtual void onPieceFailed(size_t index) = 0;

    /**
     * @brief Incremenets availability count given a peers bitfield
     * @param bitfield Bitfield of new peer
     */
    virtual void processBitfield(const std::vector<uint8_t>& bitfield) = 0;

    /**
     * @brief Decrements availability count given a peers bitfield
     * @param bitfield Bitfield of new peer
     */
    virtual void processPeerDisconnect(const std::vector<uint8_t>& bitfield) = 0;

    /**
     * @brief Increments availability count for a specific piece.
     * @param index Index of piece in have message
     */
    virtual void processHave(size_t index) = 0;
};

#endif // IPIECEPICKER_H