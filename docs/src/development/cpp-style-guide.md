# C++23 Style Guide — 2026 Edition

> **Scope:** General modern C++ best practices targeting C++23, for establishing a style guide, modernizing legacy code, and CI/tooling enforcement.
>
> **Primary authority:** [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) — supersedes Google's guide for C++23 work (Google's guide still targets C++20 as of 2026).

---

## Table of Contents

1. [Language Standard & Compiler Flags](#1-language-standard--compiler-flags)
2. [C++23 Features to Adopt](#2-c23-features-to-adopt)
3. [Core Style Rules](#3-core-style-rules)
4. [C Library Boundary Safety](#4-c-library-boundary-safety)
5. [Legacy Code Modernization](#5-legacy-code-modernization)
6. [Tooling Configuration](#6-tooling-configuration)
7. [CI Enforcement](#7-ci-enforcement)
8. [Reference Anchors](#8-reference-anchors)

---

## 1. Language Standard & Compiler Flags

Set this baseline in CMake:

```cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # Enforces -std=c++23, not -std=gnu++23
```

> **Note:** `CMAKE_CXX_EXTENSIONS OFF` is required for clang-tidy compatibility.
> GCC passes `-std=gnu++23` by default, which older clang-tidy versions reject.
> Pin clang-tidy to LLVM 19+ for full C++23 support and modern checks.

### Recommended Warnings (GCC/Clang)

```cmake
target_compile_options(mylib PRIVATE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wnon-virtual-dtor
  -Wold-style-cast -Wcast-align
  -Woverloaded-virtual
  -Wconversion -Wsign-conversion
  -Wnull-dereference
  -Wdouble-promotion
  -Wimplicit-fallthrough          # Missing break/[[fallthrough]] in switch
  -Wformat=2                      # Stricter format string checking (includes -Wformat-security)
  -Wundef                         # Undefined identifier in #if
  -Wswitch-enum                   # Switch on enum doesn't handle all values
  -Werror                         # CI only — not enforced in local dev builds
)
```

### Sanitizer Flags (Debug/Test Builds)

Enable address and undefined behavior sanitizers in Bazel via the
`--config=asan` and `--config=ubsan` blocks in `.bazelrc`:

```bash
bazel build --config=asan --config=ubsan //src/cli:sipi
# or via the wrapper:
just bazel-build-sanitized
```

The configs apply `-fsanitize=address` / `-fsanitize=undefined` together
with `-fno-omit-frame-pointer`, `-fno-optimize-sibling-calls`,
`--compilation_mode=dbg`, and `--strip=never` — DWARF must stay inline
so LSan's symbol-name suppressions in `.lsan_suppressions.txt` resolve.

Run tests under sanitizers regularly to catch memory bugs and undefined
behavior early. Thread sanitizer (`-fsanitize=thread`) is incompatible
with ASan — run it separately.

---

## 2. C++23 Features to Adopt

### Adopt Aggressively

#### `std::print` / `std::println`

Replace `printf` and `std::cout` chains entirely.

```cpp
// Before
printf("Hello %s, you are %d years old\n", name.c_str(), age);

// After (C++23)
std::println("Hello {}, you are {} years old", name, age);
```

#### `std::expected<T, E>`

Replace exception-heavy error handling for fallible operations.

```cpp
auto parse_config(std::string_view path) -> std::expected<Config, ParseError>;
```

Prefer `std::expected` over exceptions for:
- Functions that commonly fail as part of normal operation (parsing, I/O)
- API boundaries where callers need to handle errors explicitly
- Performance-sensitive paths where exception overhead matters

Continue using exceptions for:
- Truly exceptional, unrecoverable conditions
- Constructor failures

#### `std::flat_map` / `std::flat_set`

Cache-friendly replacements for `std::map`/`std::set` when iteration dominates over insertion.

```cpp
std::flat_map<std::string, int> lookup; // contiguous storage, faster iteration
```

Use `std::map`/`std::set` when you need pointer/iterator stability across mutations.

#### `if consteval`

Cleaner compile-time branching than `if constexpr`.

```cpp
constexpr int compute(int n) {
    if consteval { return fast_consteval_path(n); }
    else         { return runtime_path(n); }
}
```

#### Ranges (C++23 additions)

Use freely — the C++23 additions are well-supported in GCC 13+ and Clang 17+.

```cpp
// fold_left, contains, starts_with, ends_with, find_last, iota
auto total = std::ranges::fold_left(values, 0, std::plus{});

if (std::ranges::contains(allowed, user_input)) { ... }
```

#### Multidimensional `operator[]`

Useful for matrix, image, and tensor types.

```cpp
auto pixel = image[row, col];   // C++23 — replaces image(row, col) or image[row][col]
```

#### `std::mdspan`

Non-owning multidimensional view over contiguous data. Directly useful for image pixel buffers.

```cpp
// Wrap a raw pixel buffer as a 2D view
auto pixels = std::mdspan(raw_buffer, height, width);
auto value = pixels[row, col];   // bounds-checked with extents
```

Replaces ad-hoc `buffer[row * width + col]` indexing patterns with a type-safe abstraction. Combine with multidimensional `operator[]` for clean image processing code.

#### `std::unreachable()`

Standardized unreachable hint. Use in switch defaults after exhaustive enum handling.

```cpp
switch (direction) {
    case Direction::North: return {0, -1};
    case Direction::South: return {0, 1};
    case Direction::East:  return {1, 0};
    case Direction::West:  return {-1, 0};
}
std::unreachable();   // replaces __builtin_unreachable()
```

#### `std::to_underlying()`

Clean enum-to-integer conversion.

```cpp
// Before
auto val = static_cast<std::underlying_type_t<Color>>(color);

// After (C++23)
auto val = std::to_underlying(color);
```

#### Monadic `std::optional` operations

`and_then`, `transform`, `or_else` enable functional-style chaining.

```cpp
// Before
std::optional<Image> result;
auto file = findFile(name);
if (file) {
    auto data = loadData(*file);
    if (data) {
        result = decode(*data);
    }
}

// After (C++23)
auto result = findFile(name)
    .and_then(loadData)
    .and_then(decode);
```

#### `std::move_only_function`

Replacement for `std::function` when the callback captures move-only types.

```cpp
// ❌ Won't compile — unique_ptr is not copyable
std::function<void()> cb = [p = std::make_unique<Foo>()] { p->run(); };

// ✅ Accepts move-only captures
std::move_only_function<void()> cb = [p = std::make_unique<Foo>()] { p->run(); };
```

Prefer `std::move_only_function` over `std::function` for stored callbacks. Prefer templates for generic callback parameters.

---

### Adopt with Care

#### C++23 Modules (`import std;`)

Module support remains inconsistent across toolchains as of 2026. Header units and `import std;` interoperability across translation units is fragile. Defer unless your entire build pipeline uses CMake 3.30+ with Clang 19+/GCC 14+, and all third-party dependencies support modules.

#### `std::generator<T>` (coroutine generator)

Now supported by GCC 14+, Clang 18+, and MSVC 17.13+. Use when lazy evaluation of sequences provides clear benefits (e.g., streaming large result sets without materializing them). Ensure team familiarity with coroutine lifetime semantics — captured references and dangling is a common footgun.

#### Deducing `this` (explicit object parameter)

Eliminates CRTP boilerplate and deduplicates const/non-const overloads. Well-supported in GCC 14+, Clang 18+, MSVC 17.2+.

```cpp
// Before — duplicated const/non-const overloads
auto& get() { return data_; }
auto& get() const { return data_; }

// After — single definition, deduced value category
auto&& get(this auto&& self) { return std::forward_like<decltype(self)>(self.data_); }
```

Use for new code where it simplifies overload sets. Avoid in public API headers consumed by downstream code on older compilers.

#### `std::stacktrace`

Useful for error reporting and diagnostics. Library support varies — verify availability in your toolchain before adopting. Do not use in hot paths (capturing a stack trace is expensive).

---

### Do Not Use

- **`std::bind`** — always prefer lambdas (see [Core Style Rules](#3-core-style-rules))
- **`std::function`** for performance-sensitive callbacks — prefer templates or `std::move_only_function` (C++23)
- **VLAs** (Variable-Length Arrays) — use `std::vector` or `std::array`
- **`reinterpret_cast`** — if unavoidable, isolate and document clearly
- **Raw owning pointers** — see ownership rules below
- **`[[assume(expr)]]`** — introduces undefined behavior if the assumption is wrong; risk outweighs benefit for most code. Use `assert` for debug-only checks instead.

---

## 3. Core Style Rules

### 3.1 Naming Conventions

| Entity | Convention | Example |
|---|---|---|
| Types / Classes | `PascalCase` | `ImageProcessor`, `ParseResult` |
| Functions / Methods | `camelCase` | `parseHeader()`, `loadFile()` |
| Local variables | `snake_case` | `pixel_count`, `file_path` |
| Parameters | `snake_case` | `buffer_size`, `input_path` |
| Constants / `constexpr` | `kPascalCase` | `kMaxRetries`, `kDefaultTimeout` |
| Enum values | `kPascalCase` | `kNotFound`, `kSuccess` |
| Private members | `trailing_underscore_` | `buffer_`, `size_` |
| Namespaces | `snake_case` | `dasch::iiif::` |
| Macros | `SCREAMING_SNAKE` | `DASCH_ASSERT(x)` |
| Template parameters | `PascalCase` | `template <typename ValueType>` |
| Concepts | `PascalCase` (adjective/predicate) | `Sortable`, `Hashable`, `Renderable` |

**Minimize macros.** Prefer `constexpr`, `inline constexpr`, `[[nodiscard]]`, and templates.

---

### 3.2 Headers

**File extensions.** First-party C++ sources use `.cpp`; headers use `.h`. Do not
introduce `.cc`, `.cxx`, `.hpp`, or `.hh` for first-party code. This matches the
existing tree and the tooling in §6 (the `.clang-format` / `.clang-tidy`
invocations glob `*.cpp` and `*.h`, so any other extension silently escapes
formatting and lint). Google's `.cc` convention was rejected — renaming the whole
tree buys nothing against the industry-dominant `.cpp`. Vendored third-party
single headers keep their upstream extension (e.g. `CLI11.hpp`). The separate
question of file-*name casing* (`SipiCache.cpp` vs `cache.cpp`) is deferred to its
own ADR.

- Always use `#pragma once` — not include guards (simpler, universally supported)
- Headers must be self-contained (compilable on their own)
- Use forward declarations aggressively to reduce compile times
- Organize includes in this order, each group alphabetically sorted:
  1. Own header (for `.cpp` files)
  2. Standard library
  3. Third-party

```cpp
#pragma once

// Own
#include "image_processor.h"

// Standard
#include <expected>
#include <span>
#include <string_view>

// Third-party
#include <spdlog/spdlog.h>
```

---

### 3.3 `auto` Usage

Adopt an "almost always auto" policy with these guardrails:

```cpp
auto x = 42;                        // ✅ — type is obvious
auto result = parse_header(buf);    // ✅ — name conveys intent
auto it = container.begin();        // ✅ — iterator boilerplate
int x = compute();                  // ✅ — explicit when preferred
std::string s = getView();          // ✅ — explicit to prevent accidental string_view binding
```

Use trailing return types for non-trivial function signatures:

```cpp
auto loadFile(std::string_view path) -> std::expected<Data, IOError>;
auto transform(std::span<const float> input) -> std::vector<float>;
```

---

### 3.4 Ownership & Memory

Follow the C++ Core Guidelines ownership model. Raw owning pointers are banned.

```cpp
// ❌ Banned
Foo* p = new Foo();
delete p;

// ✅ Unique ownership (default choice)
auto obj = std::make_unique<Foo>();

// ✅ Shared ownership (use sparingly — only when ownership is genuinely shared)
auto shared = std::make_shared<Foo>();

// ✅ Non-owning observer pointer (OK — not stored, not freed)
void process(const Image* img);

// ✅ Prefer value semantics when lifetime is clear
Foo obj;
```

**Prefer value semantics first.** Reach for heap allocation only when you need polymorphism, ownership transfer, or lifetime extension.

For non-memory resources (file handles, sockets, locks), use RAII wrappers — `std::unique_ptr` with a custom deleter or a dedicated RAII class:

```cpp
// RAII for non-memory resources
auto file = std::unique_ptr<FILE, decltype(&fclose)>(fopen("data.bin", "rb"), &fclose);
```

---

### 3.5 Move Semantics & Special Member Functions

Follow the **Rule of Zero** by default — prefer value members so the compiler generates correct copy/move/destructor automatically. If you must manage a resource directly, follow the **Rule of Five**.

```cpp
// ✅ Rule of Zero — compiler generates everything correctly
class ImageMetadata {
    std::string title_;
    std::vector<std::string> tags_;
    std::shared_ptr<IccProfile> icc_;
    // No need for destructor, copy ctor, move ctor, copy assign, move assign
};

// ✅ Rule of Five — when manual resource management is unavoidable
class PixelBuffer {
public:
    PixelBuffer(size_t size);
    ~PixelBuffer();
    PixelBuffer(const PixelBuffer& other);
    PixelBuffer(PixelBuffer&& other) noexcept;           // must be noexcept
    PixelBuffer& operator=(const PixelBuffer& other);
    PixelBuffer& operator=(PixelBuffer&& other) noexcept; // must be noexcept
};
```

**Rules:**
- Mark move constructors and move assignment operators `noexcept` — this enables `std::vector` move optimizations and is required for strong exception safety
- Use `= default` for special member functions when possible
- Use `= delete` to explicitly prevent copying when a type should be move-only
- After a move, the source object must be in a valid-but-unspecified state (C++ Core Guidelines C.64)
- Assignment operators must handle self-assignment correctly

---

### 3.6 API Boundaries: Prefer Views

Use `std::span` and `std::string_view` at function boundaries — zero-copy, works with any contiguous range.

```cpp
// ❌ Forces allocation or copy
void process(const std::vector<uint8_t>& data);
void log(const std::string& msg);

// ✅ View — no copy, works with vector, array, raw buffer, string literal
void process(std::span<const uint8_t> data);
void log(std::string_view msg);
```

---

### 3.7 `const` Correctness

Be rigorous. Apply `const` everywhere it is valid.

```cpp
[[nodiscard]] auto getName() const -> const std::string&;
[[nodiscard]] auto size() const noexcept -> std::size_t;

void process(const Config& config);        // const ref for input params
```

---

### 3.8 `[[nodiscard]]`

Apply to all functions where ignoring the return value is almost certainly a bug.

```cpp
[[nodiscard]] auto loadFile(std::string_view path) -> std::expected<Data, Error>;
[[nodiscard]] auto size() const noexcept -> std::size_t;
[[nodiscard]] auto empty() const noexcept -> bool;
```

---

### 3.9 `noexcept` Policy

Mark `noexcept` when a function genuinely cannot throw. Do not mark speculatively.

**Always `noexcept`:**
- Destructors (implicit, but be explicit if non-trivial)
- Move constructors and move assignment operators
- `swap` functions
- Simple getters and observers

**Never `noexcept`:**
- Functions that allocate memory (`new` can throw `std::bad_alloc`)
- Functions that call potentially-throwing code without catching

**Be aware:** `noexcept` is part of the type system since C++17 — removing it from a public API is an ABI break.

```cpp
// ✅ Correct
~PixelBuffer() noexcept;                            // implicit, but explicit is fine
PixelBuffer(PixelBuffer&& other) noexcept;
auto size() const noexcept -> std::size_t;
friend void swap(PixelBuffer& a, PixelBuffer& b) noexcept;

// ❌ Wrong — new can throw
auto clone() noexcept -> PixelBuffer;   // don't promise what you can't guarantee
```

---

### 3.10 `enum class`

Always use `enum class` for new enumerations. Unscoped `enum` leaks names into the enclosing scope and permits implicit integer conversion.

```cpp
// ❌ Unscoped — names leak, implicit int conversion
enum Color { Red, Green, Blue };
int x = Red;   // compiles silently

// ✅ Scoped — type-safe, no name leakage
enum class Color { kRed, kGreen, kBlue };
auto val = std::to_underlying(Color::kRed);   // explicit conversion (C++23)
```

---

### 3.11 Structured Bindings

Use structured bindings to decompose tuples, pairs, and aggregates.

```cpp
// ❌ Verbose
for (auto it = map.begin(); it != map.end(); ++it) {
    process(it->first, it->second);
}

// ✅ Clean
for (const auto& [key, value] : map) {
    process(key, value);
}

// ✅ With tuple returns
auto [status, data] = fetchResult();
```

Use `const auto&` to avoid copies. Use `auto&&` when forwarding.

---

### 3.12 Concepts and Constraints

Prefer concepts over SFINAE for constraining templates (C++ Core Guidelines T.10, T.11).

```cpp
// ❌ SFINAE — hard to read, worse error messages
template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
auto process(T value) -> int;

// ✅ Concept — clear intent, readable error messages
template <std::integral T>
auto process(T value) -> int;

// ✅ requires clause for compound constraints
template <typename T>
    requires std::copyable<T> && std::regular<T>
auto store(T value) -> void;
```

**Rules:**
- Use standard library concepts (`std::integral`, `std::floating_point`, `std::invocable`, `std::ranges::range`) before defining custom ones
- Name custom concepts as adjectives or predicates: `Sortable`, `Hashable`, `Renderable`
- Prefer terse syntax (`template <std::integral T>`) over verbose `requires` when a single concept suffices

---

### 3.13 Lambdas Over `std::bind`

```cpp
// ❌ std::bind — verbose, opaque, error-prone
auto f = std::bind(&Foo::bar, this, std::placeholders::_1);

// ✅ Lambda — clear, inlineable, captures are explicit
auto f = [this](int x) { return bar(x); };
```

---

### 3.14 Error Handling Strategy

| Situation | Mechanism |
|---|---|
| Fallible operations (parsing, I/O, validation) | `std::expected<T, E>` |
| Truly unrecoverable / programming errors | `throw` / exceptions |
| Precondition violations in debug builds | `assert` / custom `ASSERT` macro |
| Performance-critical error paths | Error codes via `std::expected` |
| Resource exhaustion (OOM) | Catch `std::bad_alloc`, degrade gracefully |

Never use exceptions for control flow.

#### Exception Safety Guarantees

All functions must provide at least the **basic guarantee**. Document which functions provide stronger guarantees.

| Guarantee | Contract | When to use |
|---|---|---|
| **Nothrow** | Operation cannot fail. Mark `noexcept`. | Destructors, `swap`, move operations |
| **Strong** | Operation succeeds completely or state is unchanged (commit-or-rollback). | Mutations that can be rolled back cheaply |
| **Basic** | On exception: no leaks, all invariants preserved, but state may have changed. | Default for all functions |

RAII is the primary mechanism for achieving exception safety — destructors clean up resources regardless of how a scope is exited.

---

### 3.15 `constexpr` and Compile-Time Computation

Prefer `constexpr` over runtime computation wherever feasible:

```cpp
// ❌
#define MAX_RETRIES 5
const int kMax = 5;

// ✅
inline constexpr int kMaxRetries = 5;
constexpr auto computeBufferSize(int width, int height) -> std::size_t;
```

---

### 3.16 Concurrency and Threading

For multithreaded code, follow the C++ Core Guidelines CP section.

#### Thread Management

```cpp
// ❌ std::thread — must manually join or detach, easy to leak
std::thread t(work);
t.join();   // forgetting this is UB

// ✅ std::jthread — automatic join on destruction, cooperative cancellation
std::jthread t(work);
// automatically joined when t goes out of scope

// ✅ Cooperative cancellation via stop_token
std::jthread worker([](std::stop_token st) {
    while (!st.stop_requested()) {
        process_next_item();
    }
});
worker.request_stop();   // cooperative shutdown
```

#### Shared State Protection

```cpp
// ✅ std::scoped_lock for multiple mutexes (deadlock-free)
std::scoped_lock lock(mutex_a, mutex_b);

// ✅ std::shared_mutex for reader-heavy workloads
mutable std::shared_mutex mutex_;
auto read() const {
    std::shared_lock lock(mutex_);   // multiple readers OK
    return data_;
}
auto write(Data d) {
    std::unique_lock lock(mutex_);   // exclusive writer
    data_ = std::move(d);
}

// ✅ std::atomic for lock-free counters and flags
std::atomic<size_t> active_requests_{0};
std::atomic<bool> shutting_down_{false};
```

**Rules:**
- Never use `volatile` for synchronization (C++ Core Guidelines CP.200)
- Prefer `std::atomic` for simple counters and flags
- Prefer `std::scoped_lock` over `std::lock_guard` (handles multiple mutexes)
- Hold locks for the minimum duration necessary
- Document thread-safety guarantees in class and function documentation

---

### 3.17 Documentation

Use `///` Doxygen comments on all public API functions and classes. Inline comments should explain "why", not "what".

```cpp
/// Load an image from the filesystem.
///
/// @param path     Absolute path to the image file.
/// @param page     Zero-based page index for multi-page formats (TIFF, PDF).
/// @return         The decoded image, or an error if the file cannot be read.
/// @throws SipiError  If the file format is unsupported.
/// @note Thread-safe. May allocate up to width*height*channels bytes.
[[nodiscard]] auto loadImage(std::string_view path, int page = 0)
    -> std::expected<SipiImage, SipiError>;
```

Document:
- Preconditions and postconditions
- Thread-safety guarantees
- Exception specifications (what can throw and when)
- Non-obvious performance characteristics (allocations, I/O)

For testing conventions, see [`testing-strategy.md`](testing-strategy.md).

---

## 4. C Library Boundary Safety

Vitrinli wraps several C libraries (libtiff, libpng, libjpeg, Kakadu, lcms2, Lua). These boundaries are where the type system cannot help — mismatched types compile silently and cause undefined behavior at runtime. Follow these rules for all code that calls C library APIs.

### 4.1 Type Width at Variadic C APIs

Variadic C functions (`TIFFSetField`, `TIFFGetField`, `printf`-family) perform **zero type checking** on arguments. The caller must match the exact type the API expects.

```cpp
// ❌ Silent heap-buffer-overflow — ExtraSamples is uint8_t, libtiff reads uint16_t
TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, es.size(), es.data());

// ✅ Convert to the type the C API expects
std::vector<uint16_t> es_u16(es.begin(), es.end());
TIFFSetField(tif, TIFFTAG_EXTRASAMPLES,
             static_cast<uint16_t>(es_u16.size()), es_u16.data());
```

**Rule:** When passing enum values or typed arrays to variadic C functions, always cast to the exact documented type. Add a comment citing the C library documentation.

### 4.2 Ownership of Returned Raw Pointers

When a C++ function returns a raw pointer from `new`/`new[]`, wrap it in RAII immediately at the call site. Do not pass it through multiple function calls before freeing.

```cpp
// ❌ Leak — caller forgets to delete[], or an exception skips cleanup
kdu_byte *icc_bytes = (kdu_byte *)icc->iccBytes(icc_len);
jp2_colour.init(icc_bytes);
// icc_bytes leaked

// ✅ RAII — freed automatically regardless of control flow
auto icc_buf = std::unique_ptr<unsigned char[]>(icc->iccBytes(icc_len));
jp2_colour.init(reinterpret_cast<kdu_byte *>(icc_buf.get()));
```

**Rule:** Every raw pointer returned from `new`/`new[]` must be wrapped in `std::unique_ptr` within the same statement or the next line. If a C API takes ownership (frees the pointer itself), document this with `// ownership transferred to <api>`.

### 4.3 RAII Wrappers for C Resource Handles

C resource handles (`DIR*`, `FILE*`, `TIFF*`, `cmsHPROFILE`, `png_structp`) must use RAII so cleanup happens on all exit paths — including exceptions and early returns.

```cpp
// ❌ Leak on early return or exception — closedir never reached
DIR *dirp = opendir(path.c_str());
while (auto *dp = readdir(dirp)) { ... }
// closedir(dirp);  ← easy to forget

// ✅ RAII — closes on any exit path
auto dirp = std::unique_ptr<DIR, decltype(&closedir)>(
    opendir(path.c_str()), &closedir);
if (!dirp) throw Error("...");
while (auto *dp = readdir(dirp.get())) { ... }
// closedir called automatically
```

For handles where the deleter is complex or used repeatedly, define a named RAII wrapper:

```cpp
struct TiffDeleter { void operator()(TIFF *t) const { if (t) TIFFClose(t); } };
using TiffHandle = std::unique_ptr<TIFF, TiffDeleter>;

TiffHandle tif(TIFFOpen(path.c_str(), "r"));
```

### 4.4 Multi-Buffer Dimension Consistency

When operating on two image buffers simultaneously (e.g., watermark blending, image subtraction, compositing), every indexing expression must use dimensions from the **buffer being indexed**, not from the other buffer.

```cpp
// ❌ OOB — k iterates image channels, but indexes into watermark with fewer channels
for (size_t k = 0; k < nc; k++) {                    // nc = image channels (3)
    wm_color = bilinn(wmbuf, wm_nx, wm_ny, x, y, k, wm_nc);  // wm_nc = 1 → OOB when k≥1
}

// ✅ Clamp channel index to the target buffer's channel count
for (size_t k = 0; k < nc; k++) {
    size_t wm_k = std::min(k, static_cast<size_t>(wm_nc - 1));
    wm_color = bilinn(wmbuf, wm_nx, wm_ny, x, y, wm_k, wm_nc);
}
```

**Rule:** At every buffer indexing expression, verify that:
1. The spatial coordinates (`x`, `y`) are bounded by the buffer's width/height
2. The channel index (`c`) satisfies `c < n` where `n` is the buffer's channel count
3. The `POSITION(x, y, c, n)` macro arguments come from the same buffer

---

## 5. Legacy Code Modernization

Apply in priority order. Many are automatable with clang-tidy `--fix`.

| Old Pattern | Modern Replacement | Auto-fixable | clang-tidy Check |
|---|---|---|---|
| `NULL` / `0` as pointer | `nullptr` | Yes | `modernize-use-nullptr` |
| `typedef` | `using` | Yes | `modernize-use-using` |
| C-style casts `(T)x` | `static_cast<T>(x)` | Yes | `cppcoreguidelines-pro-type-cstyle-cast` |
| `new` / `delete` | `make_unique` / `make_shared` | Yes | `modernize-make-unique`, `modernize-make-shared` |
| Index-based `for` loops | Range-based `for` | Yes | `modernize-loop-convert` |
| `printf` / `fprintf` | `std::print` / `std::println` | Yes | `modernize-use-std-print` |
| Missing `override` | `override` keyword | Yes | `modernize-use-override` |
| Iterator algorithms | `std::ranges::` equivalents | Yes | `modernize-use-ranges` (LLVM 18+) |
| Aggregate init without field names | Designated initializers | Yes | `modernize-use-designated-initializers` (LLVM 18+) |
| `.find() == 0` | `.starts_with()` | Yes | `modernize-use-starts-ends-with` (LLVM 18+) |
| `std::bind` | Lambdas | Manual | — |
| Raw arrays `T[]` | `std::array<T,N>` / `std::vector<T>` | Manual | — |
| `throw` for expected errors | `std::expected<T,E>` | Manual | — |
| `#define` constants | `inline constexpr` | Manual | — |
| `enum` | `enum class` | Manual | `modernize-use-scoped-enum` |

**Migration strategy:** Run clang-tidy with `--fix` on automatable checks first. Review all auto-fixes before committing — they are generally correct but occasionally need adjustment near macros or third-party headers.

---

## 6. Tooling Configuration

### `.clang-format`

Place in repository root.

```yaml
---
Language: Cpp
BasedOnStyle: LLVM
Standard: Latest
IndentWidth: 4
ColumnLimit: 120
PointerAlignment: Left
ReferenceAlignment: Left
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: Never
AllowShortLambdasOnASingleLine: Inline
BraceWrapping:
  AfterFunction: true
  AfterClass: true
  AfterControlStatement: Never
SortIncludes: CaseSensitive
IncludeBlocks: Regroup
SpacesInAngles: Never
```

Run format check locally:

```bash
find src include -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror
```

Apply formatting:

```bash
find src include -name '*.cpp' -o -name '*.h' | xargs clang-format -i
```

---

### `.clang-tidy`

Place in repository root. Requires LLVM 19+ for full C++23 check coverage.

```yaml
---
Checks: >
  -*,
  bugprone-*,
  cppcoreguidelines-*,
  misc-*,
  modernize-*,
  performance-*,
  readability-*,
  portability-*,
  -modernize-use-trailing-return-type,
  -cppcoreguidelines-avoid-magic-numbers,
  -readability-magic-numbers,
  -cppcoreguidelines-pro-bounds-array-to-pointer-decay

WarningsAsErrors: >
  bugprone-*,
  cppcoreguidelines-pro-type-*,
  performance-*

CheckOptions:
  # Classes and types
  - key: readability-identifier-naming.ClassCase
    value: CamelCase
  # Functions
  - key: readability-identifier-naming.FunctionCase
    value: camelBack
  # Variables
  - key: readability-identifier-naming.VariableCase
    value: lower_case
  # Constants
  - key: readability-identifier-naming.ConstantCase
    value: CamelCase
  - key: readability-identifier-naming.ConstantPrefix
    value: "k"
  # Constexpr variables
  - key: readability-identifier-naming.ConstexprVariableCase
    value: CamelCase
  - key: readability-identifier-naming.ConstexprVariablePrefix
    value: "k"
  # Private members
  - key: readability-identifier-naming.PrivateMemberSuffix
    value: "_"
  # Namespaces
  - key: readability-identifier-naming.NamespaceCase
    value: lower_case
  # Enum constants
  - key: readability-identifier-naming.EnumConstantCase
    value: CamelCase
  - key: readability-identifier-naming.EnumConstantPrefix
    value: "k"
  # Template parameters
  - key: readability-identifier-naming.TemplateParameterCase
    value: CamelCase
  # std::print modernization
  - key: modernize-use-std-print.ReplacementPrintFunction
    value: "std::print"
  - key: modernize-use-std-print.ReplacementPrintlnFunction
    value: "std::println"

HeaderFilterRegex: '.*'
```

#### Notable Checks (LLVM 19+)

These checks are included via the wildcards above but worth calling out:

| Check | Purpose |
|-------|---------|
| `bugprone-unchecked-optional-access` | Catch `.value()` without `.has_value()` check |
| `modernize-use-ranges` | Convert iterator algorithms to ranges |
| `modernize-use-designated-initializers` | Enforce designated initializers for aggregates |
| `modernize-use-starts-ends-with` | Convert `.find() == 0` to `.starts_with()` |
| `misc-use-internal-linkage` | Suggest `static` for internal symbols |
| `cppcoreguidelines-avoid-const-or-ref-data-members` | Flag const/ref members that break assignability |
| `performance-enum-size` | Suggest smaller underlying type for enums |

Run on the whole codebase (requires `compile_commands.json`):

```bash
# Generate compile commands first (writes ./compile_commands.json)
just compile-commands

# Run linter
run-clang-tidy -p . -header-filter='.*'

# Run with auto-fix (review carefully before committing)
run-clang-tidy -p . -header-filter='.*' \
  -checks='-*,modernize-use-nullptr,modernize-use-using,modernize-make-unique' \
  -fix
```

> **Prerequisite:** `compile_commands.json` must exist in the workspace root.
> `just compile-commands` writes it (git-ignored) from Bazel's compile actions.
> Re-run it after adding or moving a C++ file or changing a target's deps.
> The nix shell's clang-tidy is LLVM 19 against the hermetic LLVM 22 compile
> commands; errors inside the toolchain's builtin headers are version skew.

---

### Build Acceleration

- **include-what-you-use (IWYU):** Complementary to clang-tidy; enforces the "self-contained headers" rule and removes unused includes
- **compile_commands.json:** Generated on demand by `just compile-commands`; see [Developing Vitrinli](developing.md#clangd)

---

## 7. CI Enforcement

### GitHub Actions

```yaml
name: Code Quality

on: [push, pull_request]

jobs:
  format:
    name: Format Check
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install clang-format
        run: sudo apt-get install -y clang-format-19
      - name: Check formatting
        run: |
          find src include -name '*.cpp' -o -name '*.h' | \
          xargs clang-format-19 --dry-run --Werror

  lint:
    name: clang-tidy
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install tools
        run: sudo apt-get install -y clang-tidy-19
      - name: Generate compile commands
        run: just compile-commands
      - name: Lint
        run: |
          run-clang-tidy-19 -p . -header-filter='.*' \
            -checks='-*,bugprone-*,modernize-*,performance-*,cppcoreguidelines-*'
```

### Rollout Strategy

1. **Format gate (Day 1):** Enforce `clang-format` as a hard PR gate. Zero discussion needed — it's mechanical.
2. **Linter warnings (Week 1-2):** Run clang-tidy in warning-only mode. Fix automatable issues in bulk using `--fix`.
3. **Linter gate (Ongoing):** Promote check groups to `WarningsAsErrors` one category at a time, starting with `bugprone-*` and `performance-*`.
4. **Modernization PRs:** Address manual migration items (error handling, ownership) as dedicated refactoring PRs — not mixed into feature work.

---

## 8. Reference Anchors

| Resource | Notes |
|---|---|
| [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines) | Primary authority for C++23 style |
| [cppreference C++23](https://en.cppreference.com/w/cpp/23) | Feature support matrix by compiler |
| [Chromium Modern C++ tracker](https://chromium.googlesource.com/chromium/src/+/main/styleguide/c++/c++-features.md) | Real-world signal on safe feature adoption |
| [OpenSSF Compiler Hardening Guide](https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html) | Compiler flags for security hardening |
| [clang-tidy checks](https://clang.llvm.org/extra/clang-tidy/checks/list.html) | Full check reference |
| [clang-format options](https://clang.llvm.org/docs/ClangFormatStyleOptions.html) | Full formatting options reference |
| [Jason Turner — C++ Best Practices](https://leanpub.com/cpp23_best_practices) | Practical C++23 guide |

---

*Last updated: March 2026*
