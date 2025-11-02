#include "bencode.h"
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <variant>
#include <iomanip> // For std::setw

/**
 * @brief Parses a Bencoded integer from the file bytes.
 * Assumes the index is currently pointing at the 'i' character.
 * Updates the index to point to the byte AFTER the final 'e'.
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 * @return A BencodeValue containing a long long.
 */
BencodeValue parseInteger(const std::vector<char>& fileBytes, size_t& index) {
  if (fileBytes[index] != 'i') {
    throw std::runtime_error("Parsing error: Expected 'i' for integer.");
  }
  index++; // Skip 'i'

  size_t start = index;
  while (index < fileBytes.size() && fileBytes[index] != 'e') {
    index++;
  }

  if (index == fileBytes.size()) {
    throw std::runtime_error("Parsing error: Unexpected EOF while parsing integer.");
  }

  //
  std::string intStr(fileBytes.begin() + start, fileBytes.begin() + index);

  if (intStr.empty()) {
    throw std::runtime_error("Parsing error: Empty integer.");
  }

  // No leading zeroes
  if (intStr.size() > 1 && intStr[0] == '0') {
    throw std::runtime_error("Parsing error: Integer has leading zero.");
  }

  // No "-0"
  if (intStr.size() > 1 && intStr[0] == '-' && intStr[1] == '0') {
    throw std::runtime_error("Parsing error: Integer is negative zero.");
  }

  index++; // Skip 'e'

  try {
    long long value = std::stoll(intStr);

    BencodeValue bv;
    bv.value = value;
    return bv;
  } catch (const std::exception& e) {
    throw std::runtime_error("Parsing error: Invalid integer format.");
  }
}

/**
 * @brief Parses a Bencoded string from the file bytes.
 * Assumes the index is currently pointing at the first digit of the length.
 * Updates the index to point to the byte AFTER the string.
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 * @return A BencodeValue containing a std::string.
 */
BencodeValue parseString(const std::vector<char>& fileBytes, size_t& index) {
  // Read the length of the string
  std::string lengthStr;
  while (index < fileBytes.size() && std::isdigit(fileBytes[index])) {
    lengthStr += fileBytes[index];
    index++;
  }

  // Length cannot be empty
  if (lengthStr.empty()) {
    throw std::runtime_error("Parsing error: String length not found.");
  }
  // Length must be followed immediately with ';'
  if (index == fileBytes.size() || fileBytes[index] != ':') {
    throw std::runtime_error("Parsing error: String length not followed by ':'.");
  }

  // Skip the ':'
  index++;

  // Convert length to a number
  // This length is length in bytes
  size_t length = std::stoull(lengthStr);

  // Check if the file is long enough
  if (index + length > fileBytes.size()) {
    throw std::runtime_error("Parsing error: Unexpected EOF before completing string.");
  }

  // Read the string content
  std::string content(fileBytes.begin() + index, fileBytes.begin() + index + length);

  // Update the index to point after the string
  index += length;

  // Wrap the string in a BencodeValue before returning
  BencodeValue bv;
  bv.value = std::move(content);
  return bv;
}

/**
 * @brief Parses a Bencoded list from the file bytes.
 * Assumes the index is currently pointing at the 'l' character.
 * Updates the index to point to the byte AFTER the final 'e'.
 * @param fileBytes The vector containing the entire file's data.
 * @return A BencodeValue containing a vector of BencodeValues.
 */
BencodeValue parseList(const std::vector<char>& fileBytes, size_t& index) {
  // Skip the 'l'
  index++;

  // BencodeList is std::vector<std::unique_ptr<BencodeValue>>
  BencodeList list;

  // loop until end of list
  // recursively decoding the bencoded list values
  while (index < fileBytes.size() && fileBytes[index] != 'e') {
    // Recursively parse the element. This returns a BencodeValue.
    BencodeValue element = parseBencodedValue(fileBytes, index);
    
    // Create unique pointer for element in list
    list.push_back(std::make_unique<BencodeValue>(std::move(element)));
  }

  // Check for a valid end
  if (fileBytes[index] != 'e') {
    throw std::runtime_error("Parsing error: List not terminated by 'e'.");
  }

  // Only skip e if not end of bencode
  if (index != fileBytes.size()) {
    // Skip the 'e'
    index++;
  }


  // Wrap in bencoded value
  BencodeValue bv;
  bv.value = std::move(list); 
  return bv; 
}

/**
 * @brief Parses a Bencoded dictionary from the file bytes.
 * Assumes the index is currently pointing at the 'd' character.
 * Updates the index to point to the byte AFTER the final 'e'.
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 */
BencodeValue parseDictionary(const std::vector<char>& fileBytes, size_t& index) {
  if (fileBytes[index] != 'd') {
    throw std::runtime_error("Parsing error: Expected 'd' for dictionary.");
  }
  index++; // Skip 'd'

  // BencodeDict is std::map<std::string, std::unique_ptr<BencodeValue>>
  BencodeDict dict;
  std::string previousKey = "";
  bool firstKey = true;

  while (index < fileBytes.size() && fileBytes[index] != 'e') {
    // Parse the Key
    if (!std::isdigit(fileBytes[index])) {
      throw std::runtime_error("Parsing error: Dictionary key is not a string.");
    }
    
    // Get string key
    // parseString returns a BencodeValue, so we must get the string out
    BencodeValue key_bv = parseString(fileBytes, index);
    std::string key = std::get<std::string>(key_bv.value);

    // Keys must be in sorted order
    if (!firstKey && key <= previousKey) {
      throw std::runtime_error("Parsing error: Dictionary keys not in sorted order.");
    }
    previousKey = key;
    firstKey = false;

    // Parse the Value
    BencodeValue value = parseBencodedValue(fileBytes, index);

    // Insert into map. The map's value type is unique_ptr<BencodeValue>,
    // so we must create one and move the parsed value into it.
    dict.emplace(std::move(key), std::make_unique<BencodeValue>(std::move(value)));
  }

  // Check for a valid end
  if (fileBytes[index] != 'e') {
    throw std::runtime_error("Parsing error: Unexpected end of file while parsing dictionary.");
  }

  // Only skip e if not end of bencode
  if (index != fileBytes.size()) {
    // Skip the 'e'
    index++;
  }
  
  // Wrap the completed map (BencodeDict) inside a
  // BencodeValue's variant and return it.
  BencodeValue bv;
  bv.value = std::move(dict);
  return bv;
}

/**
 * @brief Finds the index *after* the end of a bencoded value.
 * @param bytes The raw file bytes.
 * @param index The starting index of the value.
 * @return The index *after* the value's end.
 */
size_t findBencodedValueEnd(const std::vector<char>& bytes, size_t index) {
    if (index >= bytes.size()) {
        throw std::runtime_error("Unexpected EOF while finding end.");
    }

    char type = bytes[index];

    if (type == 'i') {
        // Integer: i...e
        while (index < bytes.size() && bytes[index] != 'e') {
            index++;
        }
        if (index == bytes.size()) {
             throw std::runtime_error("Integer not terminated.");
        }
        return index + 1; // Return index after 'e'
    } 
    else if (std::isdigit(type)) {
        // String: <len>:<data>
        std::string lengthStr;
        while (index < bytes.size() && std::isdigit(bytes[index])) {
            lengthStr += bytes[index];
            index++;
        }
        if (index == bytes.size() || bytes[index] != ':') {
            throw std::runtime_error("String length not followed by ':'.");
        }
        index++; // skip ':'
        size_t length = std::stoull(lengthStr);
        return index + length; // Return index after data
    }
    else if (type == 'l' || type == 'd') {
        // List or Dictionary: l...e or d...e
        index++; // Skip 'l' or 'd'
        while (index < bytes.size() && bytes[index] != 'e') {
            // Recursively find the end of each element inside
            index = findBencodedValueEnd(bytes, index);
        }
        if (index == bytes.size()) {
             throw std::runtime_error("List/dict not terminated.");
        }
        return index + 1; // Return index after 'e'
    }
    else {
        throw std::runtime_error("Unknown value type at " + std::to_string(index));
    }
}

/**
 * @brief router for current bencode
 * Looks at the character at the current index and calls the
 * appropriate sub-parser (e.g., parseInteger, parseString, etc.).
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 */
BencodeValue parseBencodedValue(const std::vector<char>& fileBytes, size_t& index) {
  if (index >= fileBytes.size()) {
    throw std::runtime_error("Parsing error: Unexpected end of file.");
  }

  char type = fileBytes[index];

  if (type == 'i') {
    // Integer
    return parseInteger(fileBytes, index);
  } else if (type == 'l') {
    // List
    return parseList(fileBytes, index);
  } else if (type == 'd') {
    // Dictionary
    return parseDictionary(fileBytes, index);
  } else if (std::isdigit(type)) {
    // String
    return parseString(fileBytes, index);
  } else {
    // Error
    throw std::runtime_error("Parsing error: Unknown value type.");
  }
}