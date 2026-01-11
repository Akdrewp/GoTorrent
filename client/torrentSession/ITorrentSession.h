#ifndef I_TORRENT_SESSION_H
#define I_TORRENT_SESSION_H

#include <vector>
#include <cstddef> // for size_t
#include <memory>  // for shared_ptr, weak_ptr
#include <cstdint> // for uint8_t
#include <optional>

// Forward declaration
class Peer;

/**
 * @brief Interface defining the interaction between a Peer and the Session.
 * * This interface allows for Dependency Inversion, enabling the Peer class
 * to be tested in isolation by mocking the Session
 */
class ITorrentSession {
public:
    virtual ~ITorrentSession() = default;

    // --- Callbacks (Actions the Peer performs on the Session) ---

    /**
     * @brief Called after a peer has successfully received and verified all blocks for a piece
     *
     * @param pieceIndex The zero-based index of the completed piece.
     * @param data The raw bytes of the completed piece.
     * @returns true if piece save was successful
     */
    virtual bool onPieceCompleted(size_t pieceIndex, std::vector<uint8_t> data) = 0;

    /**
     * @brief Called when a peer sends its initial BITFIELD message.
     *
     * @param peer The peer that sent the bitfield.
     * @param bitfield The raw bitfield bytes received from the peer.
     */
    virtual void onBitfieldReceived(std::shared_ptr<Peer> peer, std::vector<uint8_t> bitfield) = 0;

    /**
     * @brief Called when a peer sends a HAVE message.
     *
     * @param peer The peer that sent the message.
     * @param pieceIndex The index of the piece the peer now has.
     */
    virtual void onHaveReceived(std::shared_ptr<Peer> peer, size_t pieceIndex) = 0;

    /**
     * @brief Called by a peer when it is unchoked and ready to download data.
     *
     * @param peer The peer requesting work.
     * @return An optional piece index to download, or std::nullopt if no work is available.
     */
    virtual std::optional<size_t> assignWorkForPeer(std::shared_ptr<Peer> peer) = 0;

    /**
     * @brief Called when a peer disconnects, chokes, or fails a hash check while working on a piece.
     * This releases the lock on the piece so another peer can attempt it.
     *
     * @param pieceIndex The index of the piece to un-assign.
     */
    virtual void unassignPiece(size_t pieceIndex) = 0;


    // --- Getters (Data the Peer needs from the Session) ---

    /**
     * @return The standard length of a piece in bytes (usually defined in the .torrent metainfo).
     */
    virtual long long getPieceLength() const = 0;

    /**
     * @return The total size of the torrent's content in bytes.
     */
    virtual long long getTotalLength() const = 0;
    
    /**
     * @return A reference to the client's own bitfield (read-only).
     * Represents the pieces the client has successfully downloaded and verified.
     */
    virtual const std::vector<uint8_t>& getBitfield() const = 0;
    
    /**
     * @brief Checks if the client already possesses a specific piece.
     *
     * @param pieceIndex The index of the piece to check.
     * @return True if the client has the piece, false otherwise.
     */
    virtual bool clientHasPiece(size_t pieceIndex) const = 0;
    
    /**
     * @brief Retrieves the expected SHA-1 hash for a given piece.
     *
     * @param pieceIndex The index of the piece.
     * @return A pointer to the 20-byte hash, or nullptr if the index is invalid.
     */
    virtual const char* getPieceHash(size_t pieceIndex) const = 0;
    
    /**
     * @brief Updates the client's internal bitfield to mark a piece as complete.
     * Usually called immediately before onPieceCompleted.
     *
     * @param pieceIndex The index of the piece to mark as valid.
     */
    virtual void updateMyBitfield(size_t pieceIndex) = 0;
};

#endif // I_TORRENT_SESSION_H