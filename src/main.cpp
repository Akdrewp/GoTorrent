#include "client.h" // --- NEW: Include our main client logic ---
#include <curl/curl.h> // For curl_global_init/cleanup
#include <iostream>    // For std::cout, std::cerr
#include <stdexcept>   // For std::runtime_error

int main(int argc, char* argv[]) {
  // Initialize libcurl
  curl_global_init(CURL_GLOBAL_ALL);

  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_torrent_file>" << std::endl;
    curl_global_cleanup();
    return 1;
  }

  std::string torrentFilePath = argv[1];

  try {
    // --- Run the client logic ---
    // This function now does all the work
    runClient(torrentFilePath);

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    curl_global_cleanup(); // Clean up cURL on error
    return 1;
  }

  // Clean up libcurl
  curl_global_cleanup();
  return 0;
}

