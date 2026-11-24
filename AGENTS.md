
# Aether Engine - Codex Engineering Instructions

## 1. Project Overview

- Project: Aether Real-Time Rendering Engine
- Platform: Windows 11
- IDE: Visual Studio 2022
- Language: C++20
- Build: CMake + Ninja
- Graphics API: Vulkan
- Vulkan SDK: 1.4.357.0
- Shader Compiler: DXC

Before making changes, inspect the existing project structure,
CMake configuration, documentation and relevant source files.

Do not assume that an undocumented module or API already exists.

## 2. Architectural Principles

Strictly maintain module boundaries, ownership,
dependencies and responsibilities.

Shared access does not imply shared ownership.

Do not introduce God Managers or merge unrelated responsibilities.

Do not redesign established architecture without explicit approval.

## 3. GPU Resource Ownership

This is a critical architectural constraint.

### Image Module
- Owns VkImage creation and destruction.
- Owns ImageView creation and destruction.
- Maintains image metadata and lifecycle.
- Does not delegate Image ownership to MemoryManager.

### MemoryManager
- Manages VmaAllocator.
- Responsible only for GPU memory allocation and release.
- Must not create, destroy or own VkImage or Buffer objects.
- Must not understand Image business semantics.
- Other resource modules must use controlled MemoryManager
  interfaces rather than accessing VmaAllocator directly.

### Renderer
- Determines Image rendering usage.
- Handles image layout transitions.
- Manages dynamic rendering bindings.

### GraphicsPipeline
- Owns graphics pipeline configuration.
- Handles pipeline states such as depth testing.

Never violate these boundaries for implementation convenience.

## 4. Development Rules

- Prefer small, incremental and reviewable changes.
- Avoid unnecessary refactoring.
- Preserve existing naming conventions and coding style.
- Do not introduce new dependencies without approval.
- Do not silently change public interfaces.
- Do not delete existing modules or assets without approval.
- Inspect call sites before changing interfaces.
- Explain architectural decisions before major changes.

## 5. Build and Validation

- Inspect existing CMake presets and build configuration first.
- Use the existing Windows CMake/Ninja environment.
- Do not overwrite existing build configurations.
- Build after meaningful code changes.
- Run available tests where applicable.
- Inspect compiler errors and warnings.
- Never claim successful validation without actually running it.

## 6. Workflow

For non-trivial tasks:

1. Inspect relevant source files.
2. Explain the current implementation.
3. Identify problems and propose a solution.
4. List files that will be modified.
5. Implement the smallest reasonable change.
6. Build and validate.
7. Summarize changes, risks and remaining issues.

Ask for approval before major architectural changes.

## 7. Multi-Agent Collaboration (Antigravity & Codex)

Aether is developed collaboratively by **Antigravity** and **Codex**.
Both agents MUST obey Sections 1–6 identically; no agent may bypass
architectural or GPU resource ownership rules.

### Shared Source of Truth
- `AGENTS.md`: Binding engineering contract and collaboration protocol.
- `ARCHITECTURE.md`: Authoritative overview of the rendering pipeline,
  module layers, descriptor bindings, and resource lifetimes. Any change
  that adds a render pass, resource owner, or subsystem MUST update
  `ARCHITECTURE.md` in the same task.
- `README.md` & `Docs/VulkanValidation.md`: Build instructions, controls,
  and Vulkan validation/isolation notes.

### Role Division & Coordination
- **Antigravity**: Architecture analysis & documentation (`ARCHITECTURE.md`),
  cross-module code review, test coverage expansion, standalone CPU/asset
  modules, and build/test verification.
- **Codex**: Focused C++20/Vulkan feature slices, incremental refactoring,
  bug fixes, and targeted RHI/shader implementation.
- Roles are complementary, not rigid; whichever agent picks up a task
  owns it end-to-end (inspect → implement → build → test → summarize).

### Handoff & Workspace Hygiene Protocol
1. **Check Workspace State First**: Before editing, run `git status --short`
   and inspect recent diffs so neither agent overwrites or reverts the
   other's in-progress changes.
2. **Respect Active Build Directories**: Prefer the existing configured
   Ninja build directory (`out/build/x64-Debug`, or whichever directory
   contains the latest `build.ninja`) instead of wiping caches or creating
   duplicate build trees. Always use `chcp 65001` in VS x64 Developer
   shells so localized MSVC `/showIncludes` stays consistent.
3. **Avoid Interface Surprises**: Do not change public headers,
   `CMakeLists.txt` targets, or shader bindings silently. When an interface
   change is required, inspect all call sites and document the contract
   change in the handoff summary.
4. **Structured Handoff Summary**: Every completed task must report:
   - Modified / added files (with paths)
   - Build directory and exact build/test commands executed
   - Verification status (distinguishing CPU unit tests vs. GPU/WSI tests)
   - Remaining risks, known limitations, or follow-up items for the next agent
5. **No Conflicting Concurrent Edits**: When splitting work between
   Antigravity and Codex, assign disjoint files or modules (e.g., an
   independent `Engine/Assets/` importer + `Tests/` target vs.
   `Engine/RHI/Vulkan/` renderer passes).

