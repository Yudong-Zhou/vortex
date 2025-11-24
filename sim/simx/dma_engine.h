// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <simobject.h>
#include "types.h"

namespace vortex {

class RAM;  // Forward declaration
class Core; // Forward declaration

class DmaEngine : public SimObject<DmaEngine> {
public:
    struct PerfStats {
        uint64_t reads;
        uint64_t writes;
        uint64_t latency;

        PerfStats() : reads(0), writes(0), latency(0) {}

        PerfStats& operator+=(const PerfStats& other) {
            reads += other.reads;
            writes += other.writes;
            latency += other.latency;
            return *this;
        }
    };

    DmaEngine(const SimContext& ctx, const char* name);
    ~DmaEngine();

    void reset();
    void tick();

    // Public interface for triggering DMA transfer from execute.cpp
    void trigger_transfer(uint64_t dst_addr, uint64_t src_addr, uint64_t size, int direction);

    const PerfStats& perf_stats() const;

    void attach_ram(RAM* ram);   // For Global Memory access
    void attach_core(Core* core); // For Local Memory access

private:
    RAM* ram_;   // Direct pointer to RAM for Global Memory access
    Core* core_; // Pointer to Core for Local Memory access
    PerfStats perf_stats_;
};

} // namespace vortex

