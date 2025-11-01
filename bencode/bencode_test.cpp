#include "bencode.h" // Include the code we want to test
#include <gtest/gtest.h> // Include the GoogleTest framework
#include <variant> // Include for std::get

// TEST(TestSuiteName, TestName)

// --- Test Case Valid parseInteger ---

TEST(ParserTest, shouldParseInteger42) {
  // Create our test data
  std::string data = "i42e";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  // Run the function
  BencodeValue result_bv = parseInteger(bytes, index);
  long long result = std::get<long long>(result_bv.value);

  // Check the result
  ASSERT_EQ(result, 42);
  ASSERT_EQ(index, 4); // Make sure the index moved to the end
}

TEST(ParserTest, shouldPaseNegativeInteger42) {
  std::string data = "i-42e";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseInteger(bytes, index);
  long long result = std::get<long long>(result_bv.value);

  ASSERT_EQ(result, -42);
  ASSERT_EQ(index, 5);
}

TEST(ParserTest, shouldParseZero) {
  std::string data = "i0e";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseInteger(bytes, index);
  long long result = std::get<long long>(result_bv.value);

  ASSERT_EQ(result, 0);
  ASSERT_EQ(index, 3);
}

// --- Test Cases Invalid Errors parseInteger ---

TEST(ParserTest, shouldThrowErrorLeadingZero) {
  std::string data = "i04e";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  ASSERT_THROW(parseInteger(bytes, index), std::runtime_error);
}

TEST(ParserTest, shouldThrowErrorNegativeZero) {
  std::string data = "i-0e";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;
  
  ASSERT_THROW(parseInteger(bytes, index), std::runtime_error);
}

// --- Test Case Valid parseString ---

TEST(ParserTest, shouldParseSimpleString) {
  std::string data = "5:hello";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseString(bytes, index);
  std::string result = std::get<std::string>(result_bv.value);

  ASSERT_EQ(result, "hello");
  ASSERT_EQ(index, 7);
}

TEST(ParserTest, shouldParseEmptyString) {
  std::string data = "0:";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseString(bytes, index);
  std::string result = std::get<std::string>(result_bv.value);

  ASSERT_EQ(result, "");
  ASSERT_EQ(index, 2);
}

// --- Test Cases for Errors parseString ---

TEST(ParserTest, shouldThrowErrorStringNegativeLength) {
  // Negative length string
  std::string data = "-5:hello";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  try {
    parseString(bytes, index);
    FAIL() << "Expected std::runtime_error for negative length";
  } catch (const std::runtime_error& e) {
    // Assert correct error message
    ASSERT_STREQ("Parsing error: String length not found.", e.what());
  } catch (...) {
    FAIL() << "Expected std::runtime_error, but got different exception.";
  }
}

TEST(ParserTest, shouldThrowErrorStringMissingColon) {
  // String with no colon
  std::string data = "5hello";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  try {
    parseString(bytes, index);
    FAIL() << "Expected std::runtime_error";
  }
  catch (const std::runtime_error& e) {
    // Assert correct error message
    ASSERT_STREQ("Parsing error: String length not followed by ':'.", e.what());
  }
  catch (...) {
    FAIL() << "Expected std::runtime_error, but got different exception.";
  }
}

TEST(ParserTest, shouldThrowErrorStringUnexpectedEOF) {
  // Length is 5, but data is 4
  std::string data = "5:hell";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  try {
    parseString(bytes, index);
    FAIL() << "Expected std::runtime_error for unexpected EOF";
  } catch (const std::runtime_error& e) {
    // Assert correct error message
    ASSERT_STREQ("Parsing error: Unexpected EOF before completing string.", e.what());
  } catch (...) {
    FAIL() << "Expected std::runtime_error, but got different exception.";
  }
}

// --- TODO: Add Tests for parseList and parseDictionary ---

TEST(ParserTest, shouldParseEmptyList) {
  // Empty list
  std::string data = "le";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  // Use the router function
  BencodeValue result_bv = parseBencodedValue(bytes, index);
  
  // Check that it's a list
  ASSERT_TRUE(std::holds_alternative<BencodeList>(result_bv.value));
  
  // Get the list from the variant
  BencodeList& list = std::get<BencodeList>(result_bv.value);

  // Check size
  ASSERT_EQ(list.size(), 0);
  // Check index
  ASSERT_EQ(index, 2);
}

TEST(ParserTest, shouldParseListWithOneElement) {
  // List with one element integer 42
  std::string data = "li42ee";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  // We can call parseList directly or use the router
  BencodeValue result_bv = parseList(bytes, index);
  
  // Get the list from the variant
  BencodeList& list = std::get<BencodeList>(result_bv.value);

  // Check size
  ASSERT_EQ(list.size(), 1);

  // Check first element (integer)
  // Note: list contains unique_ptr<BencodeValue>
  BencodeValue& first_val = *list[0];
  ASSERT_TRUE(std::holds_alternative<long long>(first_val.value));
  ASSERT_EQ(std::get<long long>(first_val.value), 42);

  // Check index
  ASSERT_EQ(index, 6);
}

TEST(ParserTest, shouldParseListWithMultipleElements) {
  // List with two elements integer 42 and string "hello"
  std::string data = "li42e5:helloe";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseList(bytes, index);
  
  // Get the list from the variant
  BencodeList& list = std::get<BencodeList>(result_bv.value);

  // Check size
  ASSERT_EQ(list.size(), 2);

  // Check first element (integer)
  BencodeValue& val1 = *list[0];
  ASSERT_TRUE(std::holds_alternative<long long>(val1.value));
  ASSERT_EQ(std::get<long long>(val1.value), 42);

  // Check second element (string)
  BencodeValue& val2 = *list[1];
  ASSERT_TRUE(std::holds_alternative<std::string>(val2.value));
  ASSERT_EQ(std::get<std::string>(val2.value), "hello");

  // Check index
  ASSERT_EQ(index, 13);
}

TEST(ParserTest, shouldParseNestedList) {
  // List containing "spam" and another list [i-10e]
  std::string data = "l4:spamli-10eee";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseList(bytes, index);
  
  // Get the outer list
  BencodeList& outer_list = std::get<BencodeList>(result_bv.value);
  ASSERT_EQ(outer_list.size(), 2);

  // Check first element (string)
  BencodeValue& val1 = *outer_list[0];
  ASSERT_TRUE(std::holds_alternative<std::string>(val1.value));
  ASSERT_EQ(std::get<std::string>(val1.value), "spam");

  // Check second element (list)
  BencodeValue& val2 = *outer_list[1];
  ASSERT_TRUE(std::holds_alternative<BencodeList>(val2.value));
  
  // Get the inner list
  BencodeList& inner_list = std::get<BencodeList>(val2.value);
  ASSERT_EQ(inner_list.size(), 1);

  // Check the element inside the inner list
  BencodeValue& inner_val = *inner_list[0];
  ASSERT_TRUE(std::holds_alternative<long long>(inner_val.value));
  ASSERT_EQ(std::get<long long>(inner_val.value), -10);
  
  // Check index
  ASSERT_EQ(index, 15);
}

// --- Test Cases Valid parseDictionary ---

TEST(ParserTest, shouldParseEmptyDictionary) {
  // Dictionary containing zero elements
  std::string data = "de";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  // Use the router
  BencodeValue result_bv = parseBencodedValue(bytes, index);
  
  // Check that it's a dict
  ASSERT_TRUE(std::holds_alternative<BencodeDict>(result_bv.value));
  
  // Get the dict from the variant
  BencodeDict& dict = std::get<BencodeDict>(result_bv.value);

  // Check size
  ASSERT_EQ(dict.size(), 0);
  // Check index
  ASSERT_EQ(index, 2);
}

TEST(ParserTest, shouldParseDictWithInteger) {
  // Dictionary containing one element {"key": 42}
  std::string data = "d3:keyi42ee";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseDictionary(bytes, index);
  BencodeDict& dict = std::get<BencodeDict>(result_bv.value);

  ASSERT_EQ(dict.size(), 1);
  // Use .at() which throws if key doesn't exist
  BencodeValue& val = *dict.at("key");
  
  ASSERT_TRUE(std::holds_alternative<long long>(val.value));
  ASSERT_EQ(std::get<long long>(val.value), 42);
  ASSERT_EQ(index, 11); // d 3:key i42e e
}

TEST(ParserTest, shouldParseDictWithString) {
  // Dictionary containing one element {"key": hello}
  std::string data = "d3:key5:helloe";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseDictionary(bytes, index);
  BencodeDict& dict = std::get<BencodeDict>(result_bv.value);

  ASSERT_EQ(dict.size(), 1);
  BencodeValue& val = *dict.at("key");
  
  ASSERT_TRUE(std::holds_alternative<std::string>(val.value));
  ASSERT_EQ(std::get<std::string>(val.value), "hello");
  ASSERT_EQ(index, 14); // d 3:key 5:hello e
}

TEST(ParserTest, shouldParseDictWithList) {
  // Dictionary containing one element {"key": [-10]}
  std::string data = "d3:keyli-10eee"; // d 3:key l i-10e e e
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseDictionary(bytes, index);
  BencodeDict& dict = std::get<BencodeDict>(result_bv.value);

  ASSERT_EQ(dict.size(), 1);
  BencodeValue& val = *dict.at("key");
  
  ASSERT_TRUE(std::holds_alternative<BencodeList>(val.value));
  BencodeList& list = std::get<BencodeList>(val.value);
  
  ASSERT_EQ(list.size(), 1);
  BencodeValue& inner_val = *list[0];
  ASSERT_TRUE(std::holds_alternative<long long>(inner_val.value));
  ASSERT_EQ(std::get<long long>(inner_val.value), -10);
  
  ASSERT_EQ(index, 14);
}

TEST(ParserTest, shouldParseNestedDictionary) {
  // Dictionary containing one element {"key": {"spam": 100}}
  std::string data = "d3:keyd4:spami100eee"; // d 3:key d 4:spam i100e e e
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;

  BencodeValue result_bv = parseDictionary(bytes, index);
  
  // Get outer dict
  BencodeDict& outer_dict = std::get<BencodeDict>(result_bv.value);
  ASSERT_EQ(outer_dict.size(), 1);
  
  // Get inner value
  BencodeValue& inner_val_bv = *outer_dict.at("key");
  ASSERT_TRUE(std::holds_alternative<BencodeDict>(inner_val_bv.value));

  // Get inner dict
  BencodeDict& inner_dict = std::get<BencodeDict>(inner_val_bv.value);
  ASSERT_EQ(inner_dict.size(), 1);

  // Get value from inner dict
  BencodeValue& final_val = *inner_dict.at("spam");
  ASSERT_TRUE(std::holds_alternative<long long>(final_val.value));
  ASSERT_EQ(std::get<long long>(final_val.value), 100);

  ASSERT_EQ(index, 20);
}

// --- Test Cases Invalid Errors parseDictionary ---

TEST(ParserTest, shouldThrowErrorDictKeyNotString) {
  // Key is an integer, not a string
  std::string data = "di10e5:helloe";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;
  
  // The router will call parseDictionary, which will
  // immediately see 'i' instead of a digit and throw.
  ASSERT_THROW(parseBencodedValue(bytes, index), std::runtime_error);
}

TEST(ParserTest, shouldThrowErrorDictDuplicateKeys) {
  // "key" is repeated
  std::string data = "d3:keyi1e3:keyi2ee";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;
  
  // The spec requires keys to be sorted, so duplicate
  // keys will also fail the "key <= previousKey" check.
  ASSERT_THROW(parseDictionary(bytes, index), std::runtime_error);
}

TEST(ParserTest, shouldThrowErrorDictKeysNotSorted) {
  // "beta" should come before "key"
  std::string data = "d3:keyi1e4:betai2ee";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;
  
  ASSERT_THROW(parseDictionary(bytes, index), std::runtime_error);
}

TEST(ParserTest, shouldThrowErrorDictMissingValue) {
  // The dictionary ends after the "key"
  std::string data = "d3:keye";
  std::vector<char> bytes(data.begin(), data.end());
  size_t index = 0;
  
  // This will fail when parseBencodedValue (for the value)
  // sees 'e' instead of a valid type.
  ASSERT_THROW(parseDictionary(bytes, index), std::runtime_error);
}

// This is the main function that runs all the tests
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
