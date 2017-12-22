// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEMORY_STREAM_H_
#define SRC_MEMORY_STREAM_H_

#include <string>
#include <utility>

#include "puffin/common.h"
#include "puffin/stream.h"

namespace puffin {

// A very simple class for reading and writing into memory.
class MemoryStream : public StreamInterface {
 public:
  ~MemoryStream() override = default;

  // Creates a stream for reading.
  static UniqueStreamPtr CreateForRead(const Buffer& memory);

  // Creates a stream for writing. This function will clear the |memory| if
  // |clear| is true. An instance of this class does not retain the ownership of
  // the |memory|.
  static UniqueStreamPtr CreateForWrite(Buffer* memory);

  bool GetSize(size_t* size) const override;
  bool GetOffset(size_t* offset) const override;
  bool Seek(size_t offset) override;
  bool Read(void* buffer, size_t length) override;
  bool Write(const void* buffer, size_t length) override;
  bool Close() override;

 private:
  // Ctor. Exactly one of the |read_memory| or |write_memory| should be nullptr.
  MemoryStream(const Buffer* read_memory, Buffer* write_memory);

  // The memory buffer for reading.
  const Buffer* read_memory_;

  // The memory buffer for writing. It can grow as we write into it.
  Buffer* write_memory_;

  // The current offset.
  size_t offset_;

  // True if the stream is open.
  bool open_;

  DISALLOW_COPY_AND_ASSIGN(MemoryStream);
};

}  // namespace puffin

#endif  // SRC_MEMORY_STREAM_H_
