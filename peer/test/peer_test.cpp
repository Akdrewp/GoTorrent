#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "peer.h"
#include "iTorrentSession.h"
#include "peerConnection.h"

class MockFoo {
public:
    MOCK_METHOD(void, Bar, ());
};

TEST(MyTest, MockWorks) {
    MockFoo foo;
    EXPECT_CALL(foo, Bar());
    foo.Bar();
}