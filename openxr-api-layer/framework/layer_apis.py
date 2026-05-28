# The list of OpenXR functions this layer overrides.
#
# Adding a function here regenerates dispatch.gen.{h,cpp} on the next
# build (PreBuildEvent in the .vcxproj). You then override the matching
# virtual method on OpenXrLayer (layer.h / layer.cpp) and the framework
# calls it instead of forwarding to the runtime.
#
# IMPORTANT: a small set of functions are already wired up by the
# framework and MUST NOT appear in this list. Adding them here makes
# dispatch_generator.py raise an exception that aborts the build (see
# dispatch_generator.py's sanity-check loop). The complete set is:
#
#   xrCreateInstance
#   xrDestroyInstance
#   xrGetInstanceProcAddr
#   xrEnumerateInstanceExtensionProperties
#
# You override those by just declaring the virtual method on
# OpenXrLayer; the framework already routes them. See layer.cpp for
# the xrCreateInstance override shipped with this template.
#
# NOTE: xrDestroySession is NOT in that set despite an older version of
# this comment claiming so -- the framework does NOT auto-route it. A
# layer that needs per-session teardown (we do: releasing the D3D11 GPU
# timer's query objects) must list xrDestroySession here AND override the
# virtual method, exactly like xrCreateSession. dispatch_generator.py only
# special-cases the four functions above.
#
# Per-frame functions the sandwich measures. xrEndFrame is the canonical
# entry point for a target layer's per-frame work (composition build,
# texture copies, FOV mutation, etc.) so it is the MVP target. Add
# xrWaitFrame / xrBeginFrame / xrLocateViews here later if you want to
# measure those separately — each will get its own paired ETW + CSV rows.
#
# xrCreateSession / xrDestroySession are intercepted to stand up and tear
# down the per-session D3D11 GPU timer: xrCreateSession reads the app's
# ID3D11Device from XrGraphicsBindingD3D11KHR in createInfo->next and
# builds the timestamp-query ring; xrDestroySession releases it so we
# never leak query objects onto a dying session's device.
override_functions = [
    "xrEndFrame",
    "xrCreateSession",
    "xrDestroySession",
]

# Extra OpenXR functions the layer wants to *call* on the runtime (in
# addition to anything in override_functions, which is also callable
# downstream automatically).
#
# Add only what you actually use — every entry forces the loader to
# resolve the function pointer at layer init, and a runtime that does
# not implement an unused entry would log a noisy "function not
# present" message for no reason.
requested_functions = [
    "xrGetInstanceProperties",
    "xrGetSystemProperties",
    # Swapchain plumbing (uncomment if you create your own swapchains,
    # e.g. for an overlay quad):
    # "xrCreateReferenceSpace",
    # "xrDestroySpace",
    # "xrEnumerateSwapchainFormats",
    # "xrCreateSwapchain",
    # "xrDestroySwapchain",
    # "xrEnumerateSwapchainImages",
    # "xrAcquireSwapchainImage",
    # "xrWaitSwapchainImage",
    # "xrReleaseSwapchainImage",
]

# OpenXR extensions this layer either provides itself OR consumes from
# the runtime (in which case the loader's negotiation step is what
# matters). Empty for a pass-through skeleton.
extensions = []
