#include "torrent.h" // Our new torrent logic header
#include <iostream>    // For std::cout, std::cerr
#include <iomanip>     // For std::setw, std::hex
#include <variant>     // For std::visit
#include <stdexcept>   // For std::runtime_error

// --- Printing Functions (for debugging) ---

// Forward declaration for recursion
void printBencodeValue(const BencodeValue& bv, int indent = 0);

/**
 * @brief Helper struct for using std::visit to print BencodeValue
 */
struct BencodePrinter {
    int indent;
    void operator()(long long val) const {
        std::cout << val;
    }
    void operator()(const std::string& val) const {
        std::cout << "\"";
        for(unsigned char c : val) {
            if (std::isprint(c) && c != '\\' && c != '\"') {
                std::cout << c;
            } else {
                std::cout << "\\x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::dec;
            }
        }
        std::cout << "\"";
    }
    void operator()(const BencodeList& list) const {
        std::cout << "[\n";
        for (const auto& item_ptr : list) {
            std::cout << std::string(indent + 2, ' ');
            printBencodeValue(*item_ptr, indent + 2); // Recursive call
            std::cout << ",\n";
        }
        std::cout << std::string(indent, ' ') << "]";
    }
    void operator()(const BencodeDict& dict) const {
        std::cout << "{\n";
        for (const auto& [key, val_ptr] : dict) {
            std::cout << std::string(indent + 2, ' ') << "\"" << key << "\": ";
            if (key == "pieces") {
                std::cout << "\"(... pieces data redacted ...)\"";
            } else {
                printBencodeValue(*val_ptr, indent + 2); // Recursive call
            }
            std::cout << ",\n";
        }
        std::cout << std::string(indent, ' ') << "}";
    }
};

/**
 * @brief Pretty-prints a parsed BencodeValue.
 */
void printBencodeValue(const BencodeValue& bv, int indent) {
    std::visit(BencodePrinter{indent}, bv.value);
}


int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_torrent_file>" << std::endl;
        return 1;
    }

    std::string torrentFilePath = argv[1];

    try {
        // Parse torrent file
        TorrentData torrent = parseTorrentFile(torrentFilePath);

        // --- Print the results ---

        // Print the hash
        std::cout << "--- INFO HASH ---" << std::endl;
        std::cout << std::hex << std::setfill('0');
        for (unsigned char c : torrent.infoHash) {
            std::cout << std::setw(2) << static_cast<int>(c);
        }
        std::cout << std::dec << std::endl; // Reset to decimal

        // Print the parsed data
        std::cout << "--- PARSED DATA ---" << std::endl;
        BencodeValue parsedData;
        parsedData.value = std::move(torrent.mainData); // Move data for printing
        printBencodeValue(parsedData);
        std::cout << std::endl; // Add a final newline

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
