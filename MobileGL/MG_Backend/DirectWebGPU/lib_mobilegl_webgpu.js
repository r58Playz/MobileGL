// MobileGL - MobileGL/MG_Backend/DirectWebGPU/lib_mobilegl_webgpu.js
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header
//
// JSPI suspend primitive used by the DirectWebGPU backend to turn WebGPU's async
// completion callbacks (wgpuBufferMapAsync, wgpuQueueOnSubmittedWorkDone) into
// synchronous-looking GL operations (glReadPixels, glFinish).
//
// Pattern (same as emscripten-glfw's swapBuffers yield): mobilegl_jspi_wait is
// wrapped at load time in WebAssembly.Suspending via the __postset below. When C
// calls it, JSPI suspends the entire wasm call stack (which must have been entered
// through a WebAssembly.promising export — the host's swapBuffers path in the dotnet
// build, or the standalone harness' wrapped entry). The C-side WebGPU completion
// callback then calls mobilegl_jspi_signal(), which resolves the pending promise and
// resumes the suspended stack. This needs no JSPI/ASYNCIFY link flag in this module.
//
// Only one wait may be outstanding at a time; the backend enforces that with its
// in-flight readback guard.
mergeInto(LibraryManager.library, {
  // Device's preferred canvas format: 1 = rgba8unorm, 0 = bgra8unorm. A --js-library function
  // (not EM_JS) so it survives the native-deps `emcc -r` relocatable combine into libglfw3.a.
  mobilegl_preferred_canvas_format: function () {
    try {
      return (navigator['gpu']['getPreferredCanvasFormat']() === 'rgba8unorm') ? 1 : 0;
    } catch (e) {
      return 0;
    }
  },

  // WebGPU device bootstrap. mobilegl_has_webgpu_device() is a plain (non-suspending) probe so the
  // caller can skip the suspend when a device was preinitialized in JS (preRun) — a Suspending call
  // requires a WebAssembly.promising entry, which a preRun-populated main thread is not.
  mobilegl_has_webgpu_device: function () {
    return (Module['preinitializedWebGPUDevice']) ? 1 : 0;
  },

  // Acquire a WebGPU device on the CALLING thread (the render worker in the threaded host) and stash
  // it in Module.preinitializedWebGPUDevice, so the synchronous emscripten_webgpu_get_device() finds
  // it. Wrapped in WebAssembly.Suspending (see __postset): JSPI suspends the wasm stack until the
  // async adapter/device request resolves. Always resolves (never rejects) — on failure the device
  // stays unset and emscripten_webgpu_get_device() reports the error path.
  mobilegl_acquire_webgpu_device: function () {
    return new Promise(function (resolve) {
      if (Module['preinitializedWebGPUDevice']) { resolve(); return; }
      if (typeof navigator === 'undefined' || !navigator['gpu']) {
        console.error('[mobilegl] WebGPU (navigator.gpu) unavailable on this thread');
        resolve(); return;
      }
      navigator['gpu'].requestAdapter().then(function (adapter) {
        if (!adapter) throw new Error('no WebGPU adapter');
        // Optional features MobileGL/Iris float render targets use, requested only when available.
        var wanted = ['rg11b10ufloat-renderable', 'float32-filterable'];
        var requiredFeatures = wanted.filter(function (f) { return adapter.features.has(f); });
        return adapter.requestDevice({ requiredFeatures: requiredFeatures });
      }).then(function (device) {
        Module['preinitializedWebGPUDevice'] = device;
        resolve();
      }).catch(function (e) {
        console.error('[mobilegl] WebGPU device acquisition failed: ' + (e && e.stack || e));
        resolve();
      });
    });
  },
  mobilegl_acquire_webgpu_device__postset:
    '_mobilegl_acquire_webgpu_device = new WebAssembly.Suspending(_mobilegl_acquire_webgpu_device);',

  mobilegl_jspi_wait: function () {
    return new Promise(function (resolve) {
      Module['__mobileglJspiResolve'] = resolve;
    });
  },
  mobilegl_jspi_wait__postset:
    '_mobilegl_jspi_wait = new WebAssembly.Suspending(_mobilegl_jspi_wait);',

  mobilegl_jspi_signal: function () {
    var resolve = Module['__mobileglJspiResolve'];
    Module['__mobileglJspiResolve'] = null;
    if (resolve) resolve();
  },
});
