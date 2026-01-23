#ifndef ICHOKINGALGORITHM_H
#define ICHOKINGALGORITHM_H

#include <vector>
#include <memory>
#include "peer.h"

class IChokingAlgorithm {
public:
    virtual ~IChokingAlgorithm() = default;

    /**
     * @brief Evaluates all peers and decides who gets choked and unchoked.
     * @param peers The list of currently connected peers.
     */
    virtual void rechoke(std::vector<std::shared_ptr<Peer>>& peers) = 0;
};

#endif