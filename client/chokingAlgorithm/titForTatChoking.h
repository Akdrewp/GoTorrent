#include "IChokingAlgorithm.h"
#include <algorithm>
#include <random>
#include <spdlog/spdlog.h>

class TitForTatChoking : public IChokingAlgorithm {
public:


  /**
   * @brief Choking algorithm following
   * https://wiki.theory.org/BitTorrentSpecification specifcation
   * of tit for tat algorithm.
   * 
   * 1. Sorts the peers by their upload speed
   * 2. Unchokes the top 4
   * 3. Optimistically unchokes one random
   * peer out of rest of peers
   */
  void rechoke(std::vector<std::shared_ptr<Peer>>& peers) override {
    if (peers.empty()) return;

    // 1. Sort peers by Download Speed (Fastest -> Slowest)
    // Note:TODO if we are SEEDING, 
    // we should sort by download speed instead.
    std::sort(peers.begin(), peers.end(), [](const auto& a, const auto& b) {
      return a->getUploadRate() > b->getUploadRate();
    });

    // 2. Unchoke top 4 uploaders.
    // Put the rest in a list for potential optimistic unchoke
    size_t regularSlots = 4;
    std::vector<std::shared_ptr<Peer>> chokedPeers;
    for (size_t i = 0; i < peers.size(); ++i) {
      auto& peer = peers[i];
      if (i < regularSlots) {
        if (peer->isAmChoking()) {
          spdlog::info("Unchoking Peer (Top 4): {}", peer->getIp());
          peer->setAmChoking(false);
        }
      } else {
        chokedPeers.push_back(peer);
      }
    }

    // 3. Optimistically unchoke one random peer
    if (!chokedPeers.empty()) {
      static std::random_device rd;
      static std::mt19937 gen(rd());
      std::uniform_int_distribution<> dis(0, chokedPeers.size() - 1);
      
      auto& luckyPeer = chokedPeers[dis(gen)];
      if (luckyPeer->isAmChoking()) {
        spdlog::info("Unchoking Peer (Optimistic): {}", luckyPeer->getIp());
        luckyPeer->setAmChoking(false);
      }

      for (auto& p : chokedPeers) {
        if (p != luckyPeer && !p->isAmChoking()) {
          p->setAmChoking(true);
        }
      }
    }
  }
};