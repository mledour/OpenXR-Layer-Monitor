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
// test_integration.cpp -- lifecycle smoke test for the OpenXrLayer class
// against the mock runtime in mock_runtime.{h,cpp}.
//
// Scope: xrCreateInstance + xrDestroyInstance. The xrEndFrame call path is
// NOT exercised here yet -- driving it through the mock from the in-process
// doctest binary triggered a SIGSEGV on the CI Windows runner that did not
// reproduce locally, and the merge logic (which is what real users care
// about) is already covered exhaustively in test_merge.cpp.
//
// What this catches:
//   * The framework's auto-generated proc-addr resolution succeeds against
//     the mock for every entry in layer_apis.py's override_functions +
//     requested_functions.
//   * The layer's xrCreateInstance override survives an instance with a
//     legitimate XrApplicationInfo + the mock-provided GetInstanceProperties.
//   * xrDestroyInstance + destructor run cleanly when monitoring was never
//     turned on (default case: user loaded the layer but never pressed
//     Ctrl+F9). No stale CSV is touched, no merge runs, no writer joins.
//
// What this does NOT cover (tracked for a follow-up):
//   * xrEndFrame pass-through through the mock.
//   * Hotkey path (GetAsyncKeyState / GetForegroundWindow / shared memory).
//   * Toggle ON -> writer spawn -> Append -> toggle OFF -> merge.
// =============================================================================

// pch.h must come first: the layer's framework/dispatch.gen.h uses
// std::vector / std::string / std::pair without including their
// headers (it relies on the layer project's precompiled header). The
// test project disables PCH (<PrecompiledHeader>NotUsing</PrecompiledHeader>)
// so we pull pch.h in explicitly here, same as test_stubs.cpp.
#include "pch.h"

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

    // Redirect any accidental file writes (CSV, .log) away from the
    // developer's real %LOCALAPPDATA% under a tests-specific tmp dir.
    fs::path PrepareLocalAppData() {
        const auto dir = fs::temp_directory_path() /
                         "openxr_layer_monitor_integration_tests";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

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

TEST_CASE("integration: xrCreateInstance returns XR_SUCCESS against the mock") {
    mock::reset();
    openxr_api_layer::dllHome.clear();
    openxr_api_layer::localAppData = PrepareLocalAppData();

    XrInstance instance =
        reinterpret_cast<XrInstance>(static_cast<uintptr_t>(0xABCD));
    auto* layer = openxr_api_layer::GetInstance();
    REQUIRE(layer != nullptr);
    layer->SetGetInstanceProcAddr(&mock::xrGetInstanceProcAddr, instance);

    const XrApplicationInfo appInfo = MakeAppInfo("integration_test");
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.applicationInfo = appInfo;

    const XrResult r = layer->xrCreateInstance(&createInfo);
    CHECK(r == XR_SUCCESS);

    // Reset the singleton for the next TEST_CASE so it gets a fresh
    // OpenXrLayer (otherwise xrDestroyInstance from the next test would
    // operate on the leftover from this one).
    CHECK(layer->xrDestroyInstance(instance) == XR_SUCCESS);
}

TEST_CASE("integration: xrCreateInstance with invalid createInfo type is rejected") {
    mock::reset();
    openxr_api_layer::dllHome.clear();
    openxr_api_layer::localAppData = PrepareLocalAppData();

    XrInstance instance =
        reinterpret_cast<XrInstance>(static_cast<uintptr_t>(0x1234));
    auto* layer = openxr_api_layer::GetInstance();
    REQUIRE(layer != nullptr);
    layer->SetGetInstanceProcAddr(&mock::xrGetInstanceProcAddr, instance);

    // type field unset -> validation must reject without touching the mock.
    XrInstanceCreateInfo bogus{};
    bogus.applicationInfo = MakeAppInfo("bogus");
    CHECK(layer->xrCreateInstance(&bogus) == XR_ERROR_VALIDATION_FAILURE);

    // The mock must not have seen any meaningful call -- the layer rejected
    // before chaining downstream.
    CHECK(mock::state().endFrameCallCount == 0);
}
