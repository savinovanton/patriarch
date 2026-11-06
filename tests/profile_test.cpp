#include "pat/core/profile.hpp"

#include <doctest/doctest.h>

TEST_CASE("profile: zone macros compile and are harmless without Tracy") {
    PAT_PROFILE_FUNCTION();
    {
        PAT_PROFILE_SCOPE("test.scope");
        int sum = 0;
        for (int i = 0; i < 10; ++i) {
            sum += i;
        }
        CHECK(sum == 45);
    }
    PAT_PROFILE_FRAME();
}
