// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "puffin/src/include/puffin/utils.h"

#include <inttypes.h>

#include <string>
#include <vector>

#include <zlib.h>

#include "puffin/src/bit_reader.h"
#include "puffin/src/file_stream.h"
#include "puffin/src/include/puffin/common.h"
#include "puffin/src/include/puffin/errors.h"
#include "puffin/src/include/puffin/puffer.h"
#include "puffin/src/memory_stream.h"
#include "puffin/src/puff_writer.h"
#include "puffin/src/set_errors.h"

namespace {
// Use memcpy to access the unaligned data of type |T|.
template <typename T>
inline T get_unaligned(const void* address) {
  T result;
  memcpy(&result, address, sizeof(T));
  return result;
}

// Calculate both the compressed size and uncompressed size of the deflate
// block that starts from the offset |start| of buffer |data|.
bool CalculateSizeOfDeflateBlock(const puffin::Buffer& data,
                                 size_t start,
                                 size_t* compressed_size,
                                 size_t* uncompressed_size) {
  TEST_AND_RETURN_FALSE(compressed_size != nullptr &&
                        uncompressed_size != nullptr);

  TEST_AND_RETURN_FALSE(start < data.size());

  z_stream strm = {};
  strm.avail_in = data.size() - start;
  strm.next_in = data.data() + start;

  // -15 means we are decoding a 'raw' stream without zlib headers.
  if (inflateInit2(&strm, -15)) {
    LOG(ERROR) << "Failed to initialize inflate: " << strm.msg;
    return false;
  }

  const unsigned int kBufferSize = 32768;
  std::vector<uint8_t> uncompressed_data(kBufferSize);
  *uncompressed_size = 0;
  int status = Z_OK;
  do {
    // Overwrite the same buffer since we don't need the uncompressed data.
    strm.avail_out = kBufferSize;
    strm.next_out = uncompressed_data.data();
    status = inflate(&strm, Z_NO_FLUSH);
    if (status < 0) {
      LOG(ERROR) << "Inflate failed: " << strm.msg << ", has decompressed "
                 << *uncompressed_size << " bytes.";
      return false;
    }
    *uncompressed_size += kBufferSize - strm.avail_out;
  } while (status != Z_STREAM_END);

  *compressed_size = data.size() - start - strm.avail_in;
  TEST_AND_RETURN_FALSE(inflateEnd(&strm) == Z_OK);
  return true;
}

}  // namespace

namespace puffin {

using std::string;
using std::vector;

size_t BytesInByteExtents(const vector<ByteExtent>& extents) {
  size_t bytes = 0;
  for (const auto& extent : extents) {
    bytes += extent.length;
  }
  return bytes;
}

// This function uses RFC1950 (https://www.ietf.org/rfc/rfc1950.txt) for the
// definition of a zlib stream.
bool LocateDeflatesInZlibBlocks(const UniqueStreamPtr& src,
                                const vector<ByteExtent>& zlibs,
                                vector<BitExtent>* deflates) {
  for (auto& zlib : zlibs) {
    TEST_AND_RETURN_FALSE(src->Seek(zlib.offset));
    uint16_t zlib_header;
    TEST_AND_RETURN_FALSE(src->Read(&zlib_header, 2));
    BufferBitReader bit_reader(reinterpret_cast<uint8_t*>(&zlib_header), 2);

    TEST_AND_RETURN_FALSE(bit_reader.CacheBits(8));
    auto cmf = bit_reader.ReadBits(8);
    auto cm = bit_reader.ReadBits(4);
    if (cm != 8 && cm != 15) {
      LOG(ERROR) << "Invalid compression method! cm: " << cm;
      return false;
    }
    bit_reader.DropBits(4);
    auto cinfo = bit_reader.ReadBits(4);
    if (cinfo > 7) {
      LOG(ERROR) << "cinfo greater than 7 is not allowed in deflate";
      return false;
    }
    bit_reader.DropBits(4);

    TEST_AND_RETURN_FALSE(bit_reader.CacheBits(8));
    auto flg = bit_reader.ReadBits(8);
    if (((cmf << 8) + flg) % 31) {
      LOG(ERROR) << "Invalid zlib header on offset: " << zlib.offset;
      return false;
    }
    bit_reader.ReadBits(5);  // FCHECK
    bit_reader.DropBits(5);

    auto fdict = bit_reader.ReadBits(1);
    bit_reader.DropBits(1);

    bit_reader.ReadBits(2);  // FLEVEL
    bit_reader.DropBits(2);

    auto header_len = 2;
    if (fdict) {
      TEST_AND_RETURN_FALSE(bit_reader.CacheBits(32));
      bit_reader.DropBits(32);
      header_len += 4;
    }

    ByteExtent deflate(zlib.offset + header_len, zlib.length - header_len - 4);
    TEST_AND_RETURN_FALSE(FindDeflateSubBlocks(src, {deflate}, deflates));
  }
  return true;
}

bool FindDeflateSubBlocks(const UniqueStreamPtr& src,
                          const vector<ByteExtent>& deflates,
                          vector<BitExtent>* subblock_deflates) {
  Puffer puffer;
  Buffer deflate_buffer;
  for (const auto& deflate : deflates) {
    TEST_AND_RETURN_FALSE(src->Seek(deflate.offset));
    // Read from src into deflate_buffer.
    deflate_buffer.resize(deflate.length);
    TEST_AND_RETURN_FALSE(src->Read(deflate_buffer.data(), deflate.length));

    // Find all the subblocks.
    BufferBitReader bit_reader(deflate_buffer.data(), deflate.length);
    BufferPuffWriter puff_writer(nullptr, 0);
    Error error;
    vector<BitExtent> subblocks;
    TEST_AND_RETURN_FALSE(
        puffer.PuffDeflate(&bit_reader, &puff_writer, &subblocks, &error));
    TEST_AND_RETURN_FALSE(deflate.length == bit_reader.Offset());
    for (const auto& subblock : subblocks) {
      subblock_deflates->emplace_back(subblock.offset + deflate.offset * 8,
                                      subblock.length);
    }
  }
  return true;
}

bool LocateDeflatesInZlibBlocks(const string& file_path,
                                const vector<ByteExtent>& zlibs,
                                vector<BitExtent>* deflates) {
  auto src = FileStream::Open(file_path, true, false);
  TEST_AND_RETURN_FALSE(src);
  return LocateDeflatesInZlibBlocks(src, zlibs, deflates);
}

// For more information about the zip format, refer to
// https://support.pkware.com/display/PKZIP/APPNOTE
bool LocateDeflatesInZipArchive(const Buffer& data,
                                vector<ByteExtent>* deflate_blocks) {
  size_t pos = 0;
  while (pos <= data.size() - 30) {
    // TODO(xunchang) add support for big endian system when searching for
    // magic numbers.
    if (get_unaligned<uint32_t>(data.data() + pos) != 0x04034b50) {
      pos++;
      continue;
    }

    // local file header format
    // 0      4     0x04034b50
    // 4      2     minimum version needed to extract
    // 6      2     general purpose bit flag
    // 8      2     compression method
    // 10     4     file last modification date & time
    // 14     4     CRC-32
    // 18     4     compressed size
    // 22     4     uncompressed size
    // 26     2     file name length
    // 28     2     extra field length
    // 30     n     file name
    // 30+n   m     extra field
    auto compression_method = get_unaligned<uint16_t>(data.data() + pos + 8);
    if (compression_method != 8) {  // non-deflate type
      pos += 4;
      continue;
    }

    auto compressed_size = get_unaligned<uint32_t>(data.data() + pos + 18);
    auto uncompressed_size = get_unaligned<uint32_t>(data.data() + pos + 22);
    auto file_name_length = get_unaligned<uint16_t>(data.data() + pos + 26);
    auto extra_field_length = get_unaligned<uint16_t>(data.data() + pos + 28);
    uint64_t header_size = 30 + file_name_length + extra_field_length;

    // sanity check
    if (static_cast<uint64_t>(header_size) + compressed_size > data.size() ||
        pos > data.size() - header_size - compressed_size) {
      pos += 4;
      continue;
    }

    size_t calculated_compressed_size;
    size_t calculated_uncompressed_size;
    if (!CalculateSizeOfDeflateBlock(data, pos + header_size,
                                     &calculated_compressed_size,
                                     &calculated_uncompressed_size)) {
      LOG(ERROR) << "Failed to decompress the zip entry starting from: " << pos
                 << ", skip adding deflates for this entry.";
      pos += 4;
      continue;
    }

    // Double check the compressed size and uncompressed size if they are
    // available in the file header.
    if (compressed_size > 0 && compressed_size != calculated_compressed_size) {
      LOG(WARNING) << "Compressed size in the file header: " << compressed_size
                   << " doesn't equal the real size: "
                   << calculated_compressed_size;
    }

    if (uncompressed_size > 0 &&
        uncompressed_size != calculated_uncompressed_size) {
      LOG(WARNING) << "Uncompressed size in the file header: "
                   << uncompressed_size << " doesn't equal the real size: "
                   << calculated_uncompressed_size;
    }

    deflate_blocks->emplace_back(pos + header_size, calculated_compressed_size);
    pos += header_size + calculated_compressed_size;
  }

  return true;
}

bool LocateDeflateSubBlocksInZipArchive(const Buffer& data,
                                        vector<BitExtent>* deflates) {
  vector<ByteExtent> deflate_blocks;
  if (!LocateDeflatesInZipArchive(data, &deflate_blocks)) {
    return false;
  }

  auto src = MemoryStream::CreateForRead(data);
  return FindDeflateSubBlocks(src, deflate_blocks, deflates);
}

bool FindPuffLocations(const UniqueStreamPtr& src,
                       const vector<BitExtent>& deflates,
                       vector<ByteExtent>* puffs,
                       size_t* out_puff_size) {
  Puffer puffer;
  Buffer deflate_buffer;

  // Here accumulate the size difference between each corresponding deflate and
  // puff. At the end we add this cummulative size difference to the size of the
  // deflate stream to get the size of the puff stream. We use signed size
  // because puff size could be smaller than deflate size.
  ssize_t total_size_difference = 0;
  for (auto deflate = deflates.begin(); deflate != deflates.end(); ++deflate) {
    // Read from src into deflate_buffer.
    auto start_byte = deflate->offset / 8;
    auto end_byte = (deflate->offset + deflate->length + 7) / 8;
    deflate_buffer.resize(end_byte - start_byte);
    TEST_AND_RETURN_FALSE(src->Seek(start_byte));
    TEST_AND_RETURN_FALSE(
        src->Read(deflate_buffer.data(), deflate_buffer.size()));
    // Find the size of the puff.
    BufferBitReader bit_reader(deflate_buffer.data(), deflate_buffer.size());
    size_t bits_to_skip = deflate->offset % 8;
    TEST_AND_RETURN_FALSE(bit_reader.CacheBits(bits_to_skip));
    bit_reader.DropBits(bits_to_skip);

    BufferPuffWriter puff_writer(nullptr, 0);
    Error error;
    TEST_AND_RETURN_FALSE(
        puffer.PuffDeflate(&bit_reader, &puff_writer, nullptr, &error));
    TEST_AND_RETURN_FALSE(deflate_buffer.size() == bit_reader.Offset());

    // 1 if a deflate ends at the same byte that the next deflate starts and
    // there is a few bits gap between them. In practice this may never happen,
    // but it is a good idea to support it anyways. If there is a gap, the value
    // of the gap will be saved as an integer byte to the puff stream. The parts
    // of the byte that belogs to the deflates are shifted out.
    int gap = 0;
    if (deflate != deflates.begin()) {
      auto prev_deflate = std::prev(deflate);
      if ((prev_deflate->offset + prev_deflate->length == deflate->offset)
          // If deflates are on byte boundary the gap will not be counted later,
          // so we won't worry about it.
          && (deflate->offset % 8 != 0)) {
        gap = 1;
      }
    }

    start_byte = ((deflate->offset + 7) / 8);
    end_byte = (deflate->offset + deflate->length) / 8;
    ssize_t deflate_length_in_bytes = end_byte - start_byte;

    // If there was no gap bits between the current and previous deflates, there
    // will be no extra gap byte, so the offset will be shifted one byte back.
    auto puff_offset = start_byte - gap + total_size_difference;
    auto puff_size = puff_writer.Size();
    // Add the location into puff.
    puffs->emplace_back(puff_offset, puff_size);
    total_size_difference +=
        static_cast<ssize_t>(puff_size) - deflate_length_in_bytes - gap;
  }

  size_t src_size;
  TEST_AND_RETURN_FALSE(src->GetSize(&src_size));
  auto final_size = static_cast<ssize_t>(src_size) + total_size_difference;
  TEST_AND_RETURN_FALSE(final_size >= 0);
  *out_puff_size = final_size;
  return true;
}

}  // namespace puffin
