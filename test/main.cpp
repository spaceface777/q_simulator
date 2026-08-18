#include <cstdio>
#include <gtest/gtest.h>

namespace {

class CapturingListener : public ::testing::EmptyTestEventListener {
    void OnTestStart(const ::testing::TestInfo&) override {
        testing::internal::CaptureStdout();
        testing::internal::CaptureStderr();
    }

    void OnTestEnd(const ::testing::TestInfo& test_info) override {
        std::string stdout_capture = testing::internal::GetCapturedStdout();
        std::string stderr_capture = testing::internal::GetCapturedStderr();

        if (test_info.result()->Failed()) {
            if (!stdout_capture.empty()) {
                printf("----- captured stdout -----\n%s\n", stdout_capture.c_str());
            }
            if (!stderr_capture.empty()) {
                printf("----- captured stderr -----\n%s\n", stderr_capture.c_str());
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::UnitTest::GetInstance()->listeners().Append(new CapturingListener);
    return RUN_ALL_TESTS();
}
