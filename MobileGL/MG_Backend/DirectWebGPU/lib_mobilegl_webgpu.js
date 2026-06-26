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
