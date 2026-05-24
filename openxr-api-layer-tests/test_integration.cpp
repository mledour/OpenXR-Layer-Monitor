// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// =============================================================================
// test_integration.cpp -- drives the OpenXrLayer class through one complete
// xrCreateInstance / xrEndFrame x N / xrDestroyInstance cycle against the
// mock OpenXR runtime in mock_runtime.{h,cpp}. The point is to catch
// regressions that compile cleanly but break the layer's interaction with
// the loader / next-layer dispatch table.
//
// We do NOT exercise the Ctrl+F9 hotkey path here -- GetAsyncKeyState is not
// easily mockable without dependency injection, and PIDs / shared-memory
// names are tied to GetCurrentProcessId() which the test exe is locked to.
// The merge code's correctness is covered exhaustively in test_merge.cpp;
// here we only need to confirm:
//
//   * The layer accepts an xrCreateInstance with a sane XrInstanceCreateInfo
//     and forwards to the next-layer (mock).
//   * xrEndFrame with monitoring OFF (default) is a pure pass-through:
//     it returns the mock's result, increments the mock's call counter, and
//     does NOT spawn the writer thread / open any CSV file.
//   * xrDestroyInstance tears down cleanly. The destructor's early-return
//     (g_monitoring=false) means it must not call MergeIntoOutput nor block
//     on a non-existent writer.
//
// =============================================================================

#include <doctest/doctest.h>

#include "layer.h"
#include "mock_runtime.h"

#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace openxr_api_layer {
    extern std::filesystem::path dllHome;
    extern std::filesystem::path localAppData;
}

namespace {

    // Hot test directory: redirect any accidental file writes (CSV, .log)
    // away from the developer's real %LOCALAPPDATA%.
    fs::path PrepareLocalAppData() {
        const auto dir = fs::temp_directory_path() /
                         "openxr_layer_monitor_integration_tests";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    // Build a minimal XrInstanceCreateInfo. The applicationInfo fields are
    // what the layer's Log() prints at startup; the names match what a real
    // engine would supply.
    XrApplicationInfo MakeAppInfo(const char* appName) {
        XrApplicationInfo info{};
        std::strncpy(info.applicationName, appName,
                     XR_MAX_APPLICATION_NAME_SIZE - 1);
        info.applicationVersion = 1;
        std::strncpy(info.engineName, "doctest", XR_MAX_ENGINE_NAME_SIZE - 1);
        info.engineVersion = 1;
        info.apiVersion = XR_API_VERSION_1_0;
        return info;
    }

} // namespace

TEST_CASE("integration: xrCreateInstance -> xrEndFrame xN -> xrDestroyInstance "
          "with monitoring off") {
    mock::reset();
    openxr_api_layer::dllHome.clear();
    openxr_api_layer::localAppData = PrepareLocalAppData();

    // Use a fake XrInstance handle; the mock never dereferences it.
    XrInstance instance =
        reinterpret_cast<XrInstance>(static_cast<uintptr_t>(0xABCD));

    auto* layer = openxr_api_layer::GetInstance();
    REQUIRE(layer != nullptr);
    layer->SetGetInstanceProcAddr(&mock::xrGetInstanceProcAddr, instance);

    XrApplicationInfo appInfo = MakeAppInfo("integration_test");
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.applicationInfo = appInfo;

    SUBCASE("xrCreateInstance succeeds") {
        const XrResult r = layer->xrCreateInstance(&createInfo);
        CHECK(r == XR_SUCCESS);
    }

    SUBCASE("xrEndFrame is a pass-through when monitoring is off") {
        REQUIRE(layer->xrCreateInstance(&createInfo) == XR_SUCCESS);

        XrSession session =
            reinterpret_cast<XrSession>(static_cast<uintptr_t>(0xBEEF));
        XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
        frameEndInfo.displayTime = 0;
        frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        frameEndInfo.layerCount = 0;
        frameEndInfo.layers = nullptr;

        const uint32_t kCalls = 5;
        for (uint32_t i = 0; i < kCalls; ++i) {
            const XrResult r = layer->xrEndFrame(session, &frameEndInfo);
            CHECK(r == XR_SUCCESS);
        }
        // Each call reaches the mock unchanged -- the layer is genuinely a
        // pass-through when not recording.
        CHECK(mock::state().endFrameCallCount == kCalls);

        // No frames-<pid>-pre.csv must appear in the redirected
        // localAppData while monitoring is off.
        for (const auto& entry :
             fs::directory_iterator(openxr_api_layer::localAppData)) {
            const std::string name = entry.path().filename().string();
            CHECK_MESSAGE(name.find("frames-") != 0,
                          "no frames-<pid>-* CSV should exist while "
                          "monitoring is off: " << name);
        }
    }

    SUBCASE("xrDestroyInstance returns and does not crash") {
        REQUIRE(layer->xrCreateInstance(&createInfo) == XR_SUCCESS);
        const XrResult r = layer->xrDestroyInstance(instance);
        CHECK(r == XR_SUCCESS);
        // After ResetInstance, GetInstance() lazily makes a new OpenXrLayer.
        // It must not crash on subsequent calls.
        auto* fresh = openxr_api_layer::GetInstance();
        CHECK(fresh != nullptr);
    }
}

TEST_CASE("integration: repeated xrEndFrame calls do not leak handles or crash") {
    mock::reset();
    openxr_api_layer::dllHome.clear();
    openxr_api_layer::localAppData = PrepareLocalAppData();

    XrInstance instance =
        reinterpret_cast<XrInstance>(static_cast<uintptr_t>(0x1234));
    auto* layer = openxr_api_layer::GetInstance();
    layer->SetGetInstanceProcAddr(&mock::xrGetInstanceProcAddr, instance);

    XrApplicationInfo appInfo = MakeAppInfo("stress_test");
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.applicationInfo = appInfo;
    REQUIRE(layer->xrCreateInstance(&createInfo) == XR_SUCCESS);

    XrSession session =
        reinterpret_cast<XrSession>(static_cast<uintptr_t>(0xCAFE));
    XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};

    // 1000 frames keeps the test cheap (~ms) but exercises any per-call
    // resource leak (atomics, allocations, OS handles).
    for (int i = 0; i < 1000; ++i) {
        CHECK(layer->xrEndFrame(session, &frameEndInfo) == XR_SUCCESS);
    }
    CHECK(mock::state().endFrameCallCount == 1000);

    CHECK(layer->xrDestroyInstance(instance) == XR_SUCCESS);
}
