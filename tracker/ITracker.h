#ifndef I_TRACKER_CLIENT_H
#define I_TRACKER_CLIENT_H

#include <string>
#include <vector>
#include "tracker.h" // For PeerInfo (if needed in other methods)

/**
 * @brief Interface for communicating with the BitTorrent Tracker.
 * Allows mocking the HTTP requests for testing.
 */
class ITrackerClient {
public:
  virtual ~ITrackerClient() = default;

  /**
   * @brief Sends a GET request to the tracker.
   * @param url The fully constructed URL.
   * @return The raw bencoded response string.
   */
  virtual std::string sendRequest(const std::string& url) = 0;
};

#endif // I_TRACKER_CLIENT_H