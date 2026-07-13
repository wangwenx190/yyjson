# yyjson

A fork of [yyjson](https://github.com/ibireme/yyjson) with personal modifications.

Please be aware that this repository is for my own personal usages only and thus will never accept any kinds of feature requests and bug reports.

## Changes compared to the original project

- Removed all unused files.
- Added a header-only C++17 wrapper.
- Added a workspace for reusing reader and writer buffers.

## C++ usage

Include `yyjson.hpp` and link the existing yyjson library. The wrapper owns
documents and serialized buffers through RAII. Value and string views borrow
from their document. Keep the input buffer alive for `parse_in_situ()`.
Array and object ranges expose their size in constant time and work with
standard algorithms, including `std::ranges` algorithms in C++20 and later.

```cpp
#include <yyjson.hpp>

auto doc = yyjson::document::parse(R"({"items":[1,2,3]})");
if (doc) {
    for (auto item : doc.root()["items"].elements()) {
        auto number = item.as_uint();
        // Use number.
    }
    auto output = doc.dump(YYJSON_WRITE_PRETTY);
    // Use output.view() while output is alive.
}
```

`yyjson::mutable_document::create()` builds a document using the C arena.
`native_handle()` gives direct access to the C APIs, and `release()` passes
document ownership to the caller. The wrapper passes flags and optional error
objects through to yyjson. `dump()` uses the C++ runtime allocator by default
and also accepts a `yyjson_alc`. Keep a custom allocator's context alive until
the output is destroyed.

## Reusing buffers

Give each worker its own `yyjson_workspace` and reuse it across documents.
In C, read with `yyjson_workspace_read()` or
`yyjson_workspace_read_in_situ()`, then pass a value to
`yyjson_workspace_write()`. Use `yyjson_workspace_free()` when finished.
When compiling the library by hand, compile `src/yyjson_workspace.c` alongside
`src/yyjson.c` to use this API.

The C++ wrapper also manages the workspace lifetime.

```cpp
#include <yyjson.hpp>
#include <cstring>

auto work = yyjson::workspace::create(1024 * 1024);
char input[64] = R"({"items":[1,2,3]})";

if (work) {
    if (auto root = work.parse_in_situ(input, std::strlen(input), sizeof(input))) {
        auto output = work.dump(root);
        // Consume output before the next dump or reset.
    }
    work.reset();
}
```

A parse invalidates the previous document, and a write invalidates the previous
output. Reset invalidates both. Keep an in-situ buffer alive while its document
is in use. The cache limit bounds retained buffer capacity after reset. Active
documents and output may temporarily exceed it.

## License

```text
MIT License

Copyright (C) 2026 wangwenx190 (Yuhang ZHAO)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

This MIT license only apply to my own personal modifications, the original code of yyjson still use its original license.
