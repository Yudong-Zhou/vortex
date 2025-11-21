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

#include <simobject.h>
#include "types.h"
#include "constants.h"
#include <functional>
#include <queue>

namespace vortex {

class Socket;

class DmaEngine : public SimObject<DmaEngine> {
public:
  struct Config {
    uint32_t socket_id;
    uint32_t num_cores;
    uint32_t max_outstanding_reads;   // 最大并发读请求
    uint32_t max_outstanding_writes;  // 最大并发写请求
    uint32_t transfer_size;           // 每次传输大小(字节)
    
    Config()
      : socket_id(0)
      , num_cores(0)
      , max_outstanding_reads(4)
      , max_outstanding_writes(4)
      , transfer_size(64)  // 默认一个 cache line
    {}
  };

  struct PerfStats {
    uint64_t transfers;           // 传输次数
    uint64_t bytes_transferred;   // 传输字节数
    uint64_t cycles_active;       // 活跃周期数
    uint64_t cycles_idle;         // 空闲周期数
    uint64_t read_requests;       // 读请求数
    uint64_t write_requests;      // 写请求数
    uint64_t read_latency;        // 累计读延迟
    uint64_t write_latency;       // 累计写延迟
    uint64_t bank_conflicts;      // Bank 冲突次数
    uint64_t errors;              // 错误次数

    PerfStats()
      : transfers(0)
      , bytes_transferred(0)
      , cycles_active(0)
      , cycles_idle(0)
      , read_requests(0)
      , write_requests(0)
      , read_latency(0)
      , write_latency(0)
      , bank_conflicts(0)
      , errors(0)
    {}

    PerfStats& operator+=(const PerfStats& rhs) {
      this->transfers += rhs.transfers;
      this->bytes_transferred += rhs.bytes_transferred;
      this->cycles_active += rhs.cycles_active;
      this->cycles_idle += rhs.cycles_idle;
      this->read_requests += rhs.read_requests;
      this->write_requests += rhs.write_requests;
      this->read_latency += rhs.read_latency;
      this->write_latency += rhs.write_latency;
      this->bank_conflicts += rhs.bank_conflicts;
      this->errors += rhs.errors;
      return *this;
    }
  };

  // 内存请求/响应端口
  SimPort<MemReq> mem_req_port;   // 访问 Global Memory (通过 L1 Arbiter)
  SimPort<MemRsp> mem_rsp_port;

  SimPort<MemReq> lmem_req_port;  // 访问 Local Memory
  SimPort<MemRsp> lmem_rsp_port;

  // 完成回调类型
  using CompletionCallback = std::function<void(bool success, uint64_t bytes)>;

  DmaEngine(const SimContext& ctx, const char* name, const Config& config);
  ~DmaEngine();

  void reset();
  void tick();

  // 设置 Socket 指针(用于访问 cores)
  void set_socket(Socket* socket) {
    socket_ = socket;
  }

  // DCR 接口
  void dcr_write(uint32_t addr, uint32_t value);
  uint32_t dcr_read(uint32_t addr) const;

  // 状态查询
  bool is_busy() const;
  bool is_idle() const;
  bool is_complete() const;
  bool has_error() const;

  // 完成回调
  void set_completion_callback(CompletionCallback cb);

  // 性能统计
  PerfStats perf_stats() const;

private:
  class Impl;
  Impl* impl_;
  Socket* socket_;
};

} // namespace vortex

