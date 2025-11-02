#ifndef BENCODE_H
#define BENCODE_H

#include <vector>
#include <string>
#include <map>
#include <memory>   // For std::unique_ptr
#include <variant>  // For std::variant


// --- Type Definitions ---

// Forward declare the struct.
struct BencodeValue;

// Define the types that will be used inside the variant.
// These store pointers to BencodeValues to allow recursive variants
using BencodeList = std::vector<std::unique_ptr<BencodeValue>>;
using BencodeDict = std::map<std::string, std::unique_ptr<BencodeValue>>;

/**
 * @brief Union type of four possible bencode values
 * 
 * integer: long long
 * 
 * string: std::string
 * 
 * list: std::unique_ptr<std::vector<BencodeValue>>
 * 
 * dictionary: std::unique_ptr<std::map<std::string, BencodeValue>>
 * 
 * A BencodeValue can be ONE of these types at any given time.
 */
struct BencodeValue {
    std::variant<
        long long,
        std::string,
        BencodeList,
        BencodeDict 
    > value;
};

template<typename T>
std::unique_ptr<BencodeValue> makeBencode(T val) {
    auto bv_ptr = std::make_unique<BencodeValue>();
    bv_ptr->value = std::move(val);
    return bv_ptr;
}

// --- Parser Function Declarations ---

/**
 * @brief Parses a Bencoded integer from the file bytes.
 * Assumes the index is currently pointing at the 'i' character.
 * Updates the index to point to the byte AFTER the final 'e'.
 * 
 * form i<base10 integer>e
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 * @return A BencodeValue containing a long long.
 */
BencodeValue parseInteger(const std::vector<char>& fileBytes, size_t& index);

/**
 * @brief Parses a Bencoded string from the file bytes.
 * Assumes the index is currently pointing at the first digit of the length.
 * Updates the index to point to the byte AFTER the string.
 * 
 * form <length in base 10>:<contents>
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 * @return A BencodeValue containing a std::string.
 */
BencodeValue parseString(const std::vector<char>& fileBytes, size_t& index);

/**
 * @brief Parses a Bencoded list from the file bytes.
 * Assumes the index is currently pointing at the 'l' character.
 * Updates the index to point to the byte AFTER the final 'e'.
 * 
 * form l<bencoded values>e
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 * @return A BencodeValue containing a BencodeDict.
 */
BencodeValue parseList(const std::vector<char>& fileBytes, size_t& index);

/**
 * @brief Parses a Bencoded dictionary from the file bytes.
 * Assumes the index is currently pointing at the 'd' character.
 * Updates the index to point to the byte AFTER the final 'e'.
 * 
 * form d<bencoded string><bencoded element>e
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 * @return A BencodeValue containing the parsed element.
 */
BencodeValue parseDictionary(const std::vector<char>& fileBytes, size_t& index);

/**
 * @brief router for current bencode
 * Looks at the character at the current index and calls the
 * appropriate sub-parser (e.g., parseInteger, parseString, etc.).
 * @param fileBytes The vector containing the entire file's data.
 * @param index A reference to the current parsing position.
 */
BencodeValue parseBencodedValue(const std::vector<char>& fileBytes, size_t& index);

#endif // BENCODE_H

