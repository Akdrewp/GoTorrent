# GoTorrent

A lightweight, asynchronous BitTorrent client written in modern C++.

GoTorrent uses **Boost.Asio** for high-performance non-blocking networking and implements the core BitTorrent protocol features, allowing you to download files from .torrent metafiles.


## Features



* **Asynchronous I/O:** Built on top of Boost.Asio for handling multiple peer connections efficiently without blocking.
* **Tit-for-Tat Choking:** Implements the standard choking algorithm to optimize download speeds and reciprocate uploads.
* **Piece Picking:** Smart piece selection logic (Rarest First) to maximize swarm health.
* **Robust Logging:** Detailed, structured logging using spdlog.
* **Standard Protocol Support:** Handshakes, Bitfields, Have, Request, Piece, Keep-Alive, and Choke/Unchoke messages.


## Prerequisites

To build GoTorrent, you need a C++17 compliant compiler and the following libraries:



* **CMake** (3.10 or higher)
* **Boost** (Components: system, asio)
* **OpenSSL** (For SHA-1 hashing)
* **spdlog** (Fast C++ logging library)


## Build Instructions



1. **Clone the repository:** \
` git clone https://github.com/yourusername/GoTorrent.git `
`cd GoTorrent`

2. **Create a build directory:**
`mkdir build && cd build`

3. **Compile:**
`cmake ..`
`make`



## Usage

Run the executable passing the path to a valid .torrent file and the destination directory.

`./GoTorrent <path/to/file.torrent> <output/directory>`


**Example:**

`./GoTorrent ubuntu-22.04.torrent ./Downloads`



## Project Structure



* **Core/**: Main application logic (Session, Torrent Manager).
* **Network/**: PeerConnection and socket handling.
* **Protocol/**: Parsing logic for Bencoding and BitTorrent messages.
* **Logic/**: PiecePicker, TitForTat, and Peer state management.


## Roadmap



* [ ] **Magnet Links:** Implementation of the Distributed Hash Table (DHT) for trackerless downloads.
* [ ] **Tracker Re-announce:** Periodically refreshing peer lists from the tracker to prevent swarm attrition.
* [ ] **UPnP:** Automatic port mapping for better connectivity behind routers.
