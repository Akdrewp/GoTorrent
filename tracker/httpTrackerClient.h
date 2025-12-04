#ifndef HTTP_TRACKER_CLIENT_H
#define HTTP_TRACKER_CLIENT_H

#include "ITracker.h"
#include "tracker.h" // contains the actual global sendTrackerRequest

// Wrapper for client for dependency injection
class HttpTrackerClient : public ITrackerClient {
public:
  std::string sendRequest(const std::string& url) override {
      return sendTrackerRequest(url);
  }
};

#endif // HTTP_TRACKER_CLIENT_H