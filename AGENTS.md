# Agent Workflow Guidelines

1. Reference Samples
   - Use `ax620e_bsp_sdk/msp/sample` as the primary reference when
     creating new examples in `cpp/`.
   - Before changes, check notes in `knowledge/` (e.g.
     `ax620e_bsp_sdk_sc850sl.md`) to locate required headers,
     libraries, and resources, then consult `ax620e_bsp_sdk/`.

2. Coding Standards
   - All C++ code must follow the Google C++ Style Guide.
   - Run `clang-format --style=google` and ensure `cpplint` reports
     no issues.

3. Build Requirements
   - When adding or updating C++ sources, configure with
     `-DCMAKE_BUILD_TYPE=Contribution` so format and lint hooks run
     automatically.
   - The first build may auto-download the toolchain. Network access
     is required. Request approval before running.

4. Commit Policy
   - Do not create commits without explicit reviewer approval. Keep
     changes staged until review completes.
   - Commit messages must be a single subject line within 72 chars,
     imperative mood, no trailing period.

5. Sample Guidelines
   - Samples must not rely on library `printf`. On failures, print
     `Result` error information in the sample.
   - Do not use unclear helpers like `Try*`. Call `CmmBuffer` and
     `CmmView` APIs directly from tests or samples.
   - Do not include test IDs in variable names (e.g. `r_alloc_005`).
     Use meaningful names.
   - Insert blank lines between logical code blocks.

6. Library API Policy
   - Use `Result<T>` and `Result<void>` for all APIs. Provide error
     codes and messages via return values.
   - Remove library `printf` calls. Keep diagnostics only when
     necessary and plan a way to disable them.
   - Allow `Allocate` and `AttachExternal` only in the idle state and
     make them mutually exclusive. Determine idle by the presence of
     `alloc`.
   - Use `Free()` for owned memory. Use `DetachExternal()` for
     external buffers. Do not mix them.

7. Toolchain Bootstrap
   - `cmake/ToolchainBootstrap.cmake` downloads and configures the
     toolchain. It detects broken caches and re-fetches as needed.
   - Request approval before actions requiring network or re-download.

8. Review Workflow
   - Use Contribution builds to run formatting and linting.
   - For multi-step work, share a plan and report progress briefly.

9. Testing and Deployment
   - Build, deploy, and run tests on the device with:
     ```bash
     cmake -B build -DCMAKE_BUILD_TYPE=Contribution
     cmake --build build
     cmake --install build
     ssh ax620e-device "cd $(pwd)/build/cpp/test_libax_sys_cpp && ./test_libax_sys_cpp"
     ```
   - The test suite (`test_libax_sys_cpp`) takes approximately 1 minute
     to complete and validates CMM buffer allocation, cache operations,
     and memory mapping functionality.
   - Ensure SSH key authentication is configured (see README.md step 5)
     for passwordless deployment.
