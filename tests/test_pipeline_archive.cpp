// SPDX-License-Identifier: GPL-2.0-or-later
// Deterministic malformed-cache checks. No device or files required.
#include <cassert>
#include <limits>
#include "common/serdes.h"

int main() {
    using namespace Serialization;
    Archive data;
    Writer writer{data};
    writer.Write(std::vector<u32>{1, 2, 3});
    // Start a fresh archive cursor.
    Archive input{data.TakeOff()};
    Reader input_reader{input};
    std::vector<u32> values{99};
    input_reader.Read(values);
    assert((values == std::vector<u32>{1, 2, 3}));

    const auto rejects = [](auto write, auto read) {
        Archive encoded;
        Writer out{encoded};
        write(out);
        Archive input{encoded.TakeOff()};
        Reader in{input};
        bool rejected = false;
        try {
            read(in);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    };
    rejects([](Writer& w) { w.Write(size_t{8}); },
            [](Reader& r) { std::vector<u32> v; r.Read(v); });
    rejects([](Writer& w) { w.Write(std::numeric_limits<size_t>::max()); },
            [](Reader& r) { std::string s; r.Read(s); });
    rejects([](Writer& w) { w.Write(u8{1}); },
            [](Reader& r) { u64 v; r.Read(v); });
    // The SRT walker must check its full size before registering executable code.
    rejects([](Writer& w) { w.Write(u8{1}); },
            [](Reader& r) { r.ar.Require(2); });
}
