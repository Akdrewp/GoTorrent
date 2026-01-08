#include "diskTorrentStorage.h"
#include "bencode.h"
#include <spdlog/spdlog.h> 
#include <stdexcept>
#include <variant>
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

// Helper Log functions

static std::runtime_error DiskStorageError(const std::string& message) {
  return std::runtime_error("Storage Error: " + message);
}

// End

// --- intialize() ---

/**
 * @brief Helper for initialize 
 * Constructs file path for single and multi-file torrents.
 * @param baseDir The download directory.
 * @param head The filename (single) or root directory name (multi).
 * @param pathSegments Pointer to a list of path segments is nullptr if single file.
 * @return fs::path The fully constructed path.
 */
static fs::path getFilePath(const std::string& baseDir, const std::string& head, const BencodeList* pathSegments = nullptr) {
  fs::path fullPath = fs::path(baseDir) / head;

  if (pathSegments) {
    for (const auto& segment : *pathSegments) {
      fullPath /= segment->get<std::string>();
    }
  }

  return fullPath;
}

/**
 * @brief Helper for initialize
 * Adds a file to the passed files vector
 * * Increments currentGlobalOffset by file size
 */
static void addFileItemToFiles(
    std::vector<DiskTorrentStorage::FileEntry>& files,
    size_t& currentGlobalOffset,
    const std::unique_ptr<BencodeValue>& fileItem,
    const std::string& downloadDirectory,
    const std::string& rootDirName
) {
  auto& fileDict = fileItem->get<BencodeDict>();
  
  // Get Length
  long long fileLen = fileDict.at("length")->get<long long>();

  // Get Path (List of strings)
  auto& pathList = fileDict.at("path")->get<BencodeList>();

  // Construct full path: downloadDir / rootName / someFolder / someFile
  fs::path fullPath = getFilePath(downloadDirectory, rootDirName, &pathList);

  // Add to vector
  files.push_back({fullPath, static_cast<size_t>(fileLen), currentGlobalOffset});
  currentGlobalOffset += static_cast<size_t>(fileLen);
}

void DiskTorrentStorage::initialize(const TorrentData& torrent, long long pieceLength, const std::string& downloadDirectory) {

  auto& infoDict = torrent.mainData.at("info")->get<BencodeDict>();
  pieceLength_ = pieceLength;
  downloadDirectory_ = downloadDirectory;
  files_.clear();

  size_t currentGlobalOffset = 0;

  // 1. Determine Structure (Single vs Multi)
  if (infoDict.count("files")) {
    // --- Multi File Torrent ---
    
    std::string rootDirName = infoDict.at("name")->get<std::string>();
    auto& filesList = infoDict.at("files")->get<BencodeList>();

    for (const auto& fileItem : filesList) {
      addFileItemToFiles(files_, 
                         currentGlobalOffset, 
                         fileItem, 
                         downloadDirectory_, 
                         rootDirName);
    }

  } else if (infoDict.count("length")) {
    // --- Single File Torrent ---
    
    std::string filename = infoDict.at("name")->get<std::string>();
    long long fileLen = infoDict.at("length")->get<long long>();
    
    fs::path fullPath = getFilePath(downloadDirectory_, filename);

    files_.push_back({fullPath, static_cast<size_t>(fileLen), currentGlobalOffset});
    // currentGlobalOffset += fileLen; // Not strictly needed for single file, but good for consistency
  } else {
    throw DiskStorageError("Unknown torrent format (missing 'length' or 'files').");
  }

  // Create physical files on disk
  createFileStructure();
}

// --- initialize() END ---

// --- createFileStructure() ---

void DiskTorrentStorage::handleExistingFile(const fs::path& path) {
  // Throw error for now. May want to have option
  if (fs::exists(path)) {
    throw DiskStorageError("File already exists: " + path.string());
  }
}

void DiskTorrentStorage::createFileStructure() {
  // For each files create the directories and file path
  for (const auto& entry : files_) {
    // Create parent directories if needed
    if (entry.path.has_parent_path()) {
      fs::create_directories(entry.path.parent_path());
    }

    // Handles cases where the file already exists
    handleExistingFile(entry.path);

    // Create empty file
    std::ofstream ofs(entry.path, std::ios::binary);
    if (!ofs) throw DiskStorageError("Failed to create file: " + entry.path.string());
    // Resize fill with 0s
    fs::resize_file(entry.path, entry.length); 

  }
  spdlog::info("Storage: Initialized storage structure for {} files.", files_.size());
}

// --- createFileStructure() END ---

// --- getFileStream ---

/**
 * @brief Helper for getFileStream
 * Checks if a file stream is already open in the map.
 * If found, moves the file to the front of the pool and returns the stream.
 * If not found, returns nullptr.
 */
static std::fstream* getFileStreamFromMap(
    std::unordered_map<std::string, DiskTorrentStorage::FilePoolList::iterator>& openFilesMap,
    DiskTorrentStorage::FilePoolList& filePool,
    const std::string& pathStr
) {
  auto it = openFilesMap.find(pathStr);
  if (it != openFilesMap.end()) {
    // Move this node to the front of the list (Most Recently Used)
    filePool.splice(filePool.begin(), filePool, it->second);
    return it->second->second.get();
  }
  return nullptr;
}

std::fstream* DiskTorrentStorage::getFileStream(const std::filesystem::path& path) {
  std::string pathStr = path.string();

  // Check map
  if (std::fstream* stream = getFileStreamFromMap(openFilesMap_, filePool_, pathStr)) {
    return stream;
  }

  // Check pool capacity
  if (filePool_.size() >= MAX_OPEN_FILES) {
    // Remove least recently used
    auto lastIt = std::prev(filePool_.end());
    std::string lastPath = lastIt->first;
    
    openFilesMap_.erase(lastPath);
    filePool_.erase(lastIt); // Destructor closes the file
  }

  // Open new file
  auto stream = std::make_unique<std::fstream>();
  stream->open(path, std::ios::binary | std::ios::in | std::ios::out);
  
  if (!stream->is_open()) {
    throw DiskStorageError("Failed to open file: " + pathStr);
  }

  // Add to pool at front of list
  filePool_.push_front({pathStr, std::move(stream)});
  openFilesMap_[pathStr] = filePool_.begin();

  return filePool_.front().second.get();
}

// initializeSingleFile

/**
 * @brief Helper for initializeSingleFile()
 * * Creates the output file if it doesn't exist and opens it
 * * @param outputFile The file stream
 * @param fullPath Path to file
 */
static void createAndOpenOutputFile(std::fstream& outputFile, const fs::path& fullPath) {
  // Check if file exists. If so, throw error
  // May be handled differently perhaps by asking if user wants to overwrite
  if (fs::exists(fullPath)) {
    throw DiskStorageError("File already exists: " + fullPath.string());
  }

  spdlog::info("Storage: Creating new file...");

  // Create empty file and close
  outputFile.open(fullPath, std::ios::binary | std::ios::out);
  outputFile.close();

  // Open in Read/Write mode for random access
  outputFile.open(fullPath, std::ios::binary | std::ios::in | std::ios::out);

  if (!outputFile.is_open()) {
    throw DiskStorageError("Failed to open output file: " + fullPath.string());
  }
}

//

/**
 * @brief Static helper to process a range of bytes spanning potentially multiple files.
 * @tparam Func Lambda type: void(std::fstream* stream, const fs::path& path, size_t localOffset, size_t chunk)
 * Operation to be done on the stream, path, localOffset, and chunk of file
 * @param files The vector of file entries.
 * @param streamGetter Lambda to retrieve an open stream for a path.
 * @param globalOffset Global byte offset to start at.
 * @param length Total bytes to process.
 * @param operation The read or write operation to perform on the specific file chunk.
 */
template <typename Func>
static void processFileRange(
    const std::vector<DiskTorrentStorage::FileEntry>& files,
    std::function<std::fstream*(const std::filesystem::path&)> streamGetter,
    size_t globalOffset,
    size_t length,
    Func operation
) {
  size_t bytesRemaining = length;
  size_t currentGlobal = globalOffset;

  // For each file find the file(s) that are 
  // within length from offset and do operation
  for (const auto& file : files) {
    if (bytesRemaining == 0) break;

    size_t fileStart = file.globalOffset;
    size_t fileEnd = fileStart + file.length;

    // Check intersection
    if (currentGlobal >= fileStart && currentGlobal < fileEnd) {
      size_t localOffset = currentGlobal - fileStart;
      size_t available = file.length - localOffset;
      size_t chunk = std::min(bytesRemaining, available);

      std::fstream* stream = streamGetter(file.path);
      
      // Execute the specific operation (Read/Write)
      // We pass localOffset so the operation knows where to seek
      operation(stream, file.path, localOffset, chunk);

      currentGlobal += chunk;
      bytesRemaining -= chunk;
    }
  }

  if (bytesRemaining > 0) {
    throw DiskStorageError("Operation incomplete. Bytes remaining: " + std::to_string(bytesRemaining));
  }
}

void DiskTorrentStorage::writePiece(size_t pieceIndex, const std::vector<uint8_t>& data) {
  size_t dataOffset = 0;

  // Define how to get stream
  auto streamGetter = [this](const fs::path& p) { return this->getFileStream(p); };

  // Define write operation
  auto writeOp = [&](std::fstream* stream, const fs::path& path, size_t localOffset, size_t chunk) {
    stream->seekp(localOffset, std::ios::beg);
    if (stream->fail()) throw DiskStorageError("Seek failed in " + path.string());

    stream->write(reinterpret_cast<const char*>(data.data() + dataOffset), chunk);
    if (stream->fail()) throw DiskStorageError("Write failed in " + path.string());

    stream->flush();
    dataOffset += chunk;
  };

  // Write to pieces in range of data
  processFileRange(files_, streamGetter, pieceIndex * pieceLength_, data.size(), writeOp);
}

// --- readBlock ---

std::vector<uint8_t> DiskTorrentStorage::readBlock(size_t pieceIndex, size_t begin, size_t length) {
  std::vector<uint8_t> buffer(length);
  size_t bufferOffset = 0;

  // Define how to get stream
  auto streamGetter = [this](const fs::path& p) { return this->getFileStream(p); };

  // Define read operation
  auto readOp = [&](std::fstream* stream, const fs::path& path, size_t localOffset, size_t chunk) {
    stream->seekg(localOffset, std::ios::beg);
    if (stream->fail()) throw DiskStorageError("Seek failed during read in " + path.string());

    stream->read(reinterpret_cast<char*>(buffer.data() + bufferOffset), chunk);
    
    if (stream->fail() || static_cast<size_t>(stream->gcount()) != chunk) {
      throw DiskStorageError("Read incomplete in " + path.string());
    }
    bufferOffset += chunk;
  };

  processFileRange(files_, streamGetter, (pieceIndex * pieceLength_) + begin, length, readOp);
  
  return buffer;
}