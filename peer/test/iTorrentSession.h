#ifndef I_TORRENT_SESSION_H
#define I_TORRENT_SESSION_H

#include <vector>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <optional>

class Peer;

/**
 * @brief Interface defining the interaction between a Peer and the Session
 * 
 * This is to allow mocking and testing of Peer.h
 */
class ITorrentSession {
public:
    virtual ~ITorrentSession() = default;

    // Callbacks called by Peer
    virtual void onPieceCompleted(size_t pieceIndex, std::vector<uint8_t> data) = 0;
    virtual void onBitfieldReceived(std::shared_ptr<Peer> peer, std::vector<uint8_t> bitfield) = 0;
    virtual void onHaveReceived(std::shared_ptr<Peer> peer, size_t pieceIndex) = 0;
    virtual std::optional<size_t> assignWorkForPeer(std::shared_ptr<Peer> peer) = 0;
    virtual void unassignPiece(size_t pieceIndex) = 0;

    // Getters called by Peer
    virtual long long getPieceLength() const = 0;
    virtual long long getTotalLength() const = 0;
    virtual const std::vector<uint8_t>& getBitfield() const = 0;
    virtual bool clientHasPiece(size_t pieceIndex) const = 0;
    virtual const char* getPieceHash(size_t pieceIndex) const = 0;
    virtual void updateMyBitfield(size_t pieceIndex) = 0;
};

#endif // I_TORRENT_SESSION_H