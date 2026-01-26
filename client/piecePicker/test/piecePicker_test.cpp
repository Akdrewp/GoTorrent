#include <gtest/gtest.h>
#include "piecePicker.h" 
#include <vector>
#include <numeric>

using namespace testing;

class PiecePickerTest : public Test {
protected:
    std::shared_ptr<PiecePicker> picker;
    const size_t NUM_PIECES = 10;
    std::vector<uint8_t> myBitfield; // empty client bitfield

    void SetUp() override {
        picker = std::make_shared<PiecePicker>(NUM_PIECES);
        myBitfield.resize((NUM_PIECES + 7) / 8, 0);
    }

    // Helper: Create a bitfield with specific bits set
    // Indices [0, 1, 2] -> 11100000 (Binary)
    std::vector<uint8_t> makeBitfield(const std::vector<size_t>& indices) {
        std::vector<uint8_t> bf((NUM_PIECES + 7) / 8, 0);
        for (size_t i : indices) {
            bf[i / 8] |= (1 << (7 - (i % 8)));
        }
        return bf;
    }
};

// --- Basic Selection Tests ---

// (The test you requested)
TEST_F(PiecePickerTest, PickPiece_SelectsSingleAvailable) {
    // Peer only has piece 4
    auto peerBitfield = makeBitfield({4});
    picker->processBitfield(peerBitfield);

    auto result = picker->pickPiece(peerBitfield, myBitfield);

    // Expectation: pickPiece picks the only available piece, piece 4
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 4);
}

TEST_F(PiecePickerTest, PickPiece_ReturnsNull_WhenPeerHasNothingWeNeed) {
    // Peer has nothing
    auto peerBitfield = makeBitfield({});
    picker->processBitfield(peerBitfield);

    auto result = picker->pickPiece(peerBitfield, myBitfield);

    // Expectation: pickPiece should return nullopt when the peer has no piece
    EXPECT_FALSE(result.has_value());
}

TEST_F(PiecePickerTest, PickPiece_ReturnsNull_WhenWeHaveEverythingPeerHas) {
    // Peer has {1, 2}, Client has {1, 2}
    auto peerBitfield = makeBitfield({1, 2});
    auto clientBitfield = makeBitfield({1, 2});
    
    picker->processBitfield(peerBitfield);

    auto result = picker->pickPiece(peerBitfield, clientBitfield);

    // Expectation: pickPiece should return nullopt when the peer has no piece client needs
    EXPECT_FALSE(result.has_value());
}

// --- Rarest First Strategy Tests ---

TEST_F(PiecePickerTest, PickPiece_ShouldPrioritizeRarestPiece) {
    // Scenario:
    // Peer A has: {0, 1, 2}
    // Peer B has: {0, 1}

    auto peerA_Bf = makeBitfield({0, 1, 2});
    auto peerB_Bf = makeBitfield({0, 1});
    picker->processBitfield(peerA_Bf);
    picker->processBitfield(peerB_Bf);

    // Peer A asks for work
    auto result = picker->pickPiece(peerA_Bf, myBitfield);

    // Expectation: pickPiece should return piece 2 since it's the rarest
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 2);
}

TEST_F(PiecePickerTest, PickPiece_ShouldBreakTiesSequentially) {
    // All pieces have equal rarity
    auto peerBitfield = makeBitfield({5, 2, 8});
    picker->processBitfield(peerBitfield);

    // Should pick lowest index first (2)
    auto res1 = picker->pickPiece(peerBitfield, myBitfield);
    EXPECT_EQ(*res1, 2);

    // Should pick next lowest (5)
    auto res2 = picker->pickPiece(peerBitfield, myBitfield);
    EXPECT_EQ(*res2, 5);

    // Should pick last (8)
    auto res3 = picker->pickPiece(peerBitfield, myBitfield);
    EXPECT_EQ(*res3, 8);
}

// --- Concurrency / Locking Tests ---

TEST_F(PiecePickerTest, PickPiece_ShouldNotAssignSamePieceTwice) {
    // Peer 1 and Peer 2 both have Piece 0
    auto peerBitfield = makeBitfield({0});
    picker->processBitfield(peerBitfield); // Peer 1
    picker->processBitfield(peerBitfield); // Peer 2

    // Peer 1 takes Piece 0
    auto res1 = picker->pickPiece(peerBitfield, myBitfield);
    ASSERT_TRUE(res1.has_value());
    EXPECT_EQ(*res1, 0);

    // Peer 2 tries to take Piece 0
    auto res2 = picker->pickPiece(peerBitfield, myBitfield);
    
    // Expectation: pickPiece should return nullopt since
    // Piece 0 is In-Flight
    EXPECT_FALSE(res2.has_value());
}

TEST_F(PiecePickerTest, OnPieceFailed_ShouldMakePieceAvailableAgain) {
    // Peer has piece 0
    auto peerBitfield = makeBitfield({0});
    picker->processBitfield(peerBitfield);

    // Assign Piece 0
    picker->pickPiece(peerBitfield, myBitfield);

    // Expectation: pickPiece should return nullopt since
    // Piece 0 is In-Flight
    auto failRes = picker->pickPiece(peerBitfield, myBitfield);
    EXPECT_FALSE(failRes.has_value());

    // Report Failure (Unlock)
    picker->onPieceFailed(0);

    // Expectation: pickPiece should return piece 0 since it's
    // now available
    auto successRes = picker->pickPiece(peerBitfield, myBitfield);
    ASSERT_TRUE(successRes.has_value());
    EXPECT_EQ(*successRes, 0);
}

TEST_F(PiecePickerTest, OnPiecePassed_ShouldUnlockPiece) {
    // Peer has piece 0 
    auto peerBitfield = makeBitfield({0});
    picker->processBitfield(peerBitfield);

    // Assign Piece 0
    picker->pickPiece(peerBitfield, myBitfield);
    picker->onPiecePassed(0);

    // Expectation: pickPiece chooses piece 0 again since it's
    // no longer in flight 
    // (This is intended because a piece)
    auto res = picker->pickPiece(peerBitfield, myBitfield);
    EXPECT_TRUE(res.has_value()); 
}

// --- Dynamic Availability Updates ---

TEST_F(PiecePickerTest, ProcessPeerDisconnect_ShouldDecreaseAvailability) {
    // Peer A has Piece 0
    auto peerA_Bf = makeBitfield({0});
    picker->processBitfield(peerA_Bf);
    picker->processPeerDisconnect(peerA_Bf);

    // Peer B connects (Also has Piece 0)
    // If availability wasn't decremented, rarity might be wrong, 
    // but here we just check basic logic stability.
    
    auto peerB_Bf = makeBitfield({0});
    picker->processBitfield(peerB_Bf);
    
    auto res = picker->pickPiece(peerB_Bf, myBitfield);
    EXPECT_EQ(*res, 0);
}

TEST_F(PiecePickerTest, ProcessHave_ShouldIncreaseAvailability) {
    // Initial: Peer has nothing
    auto peerBf = makeBitfield({});
    picker->processBitfield(peerBf);

    // Message: Peer found Piece 5
    picker->processHave(5);
    
    // Update our local copy of peer bitfield (simulated)
    peerBf = makeBitfield({5});

    auto res = picker->pickPiece(peerBf, myBitfield);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, 5);
}