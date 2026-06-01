#ifndef IMMUTABLE_STRING_H
#define IMMUTABLE_STRING_H

/* Copyright (c) 2020, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
 * @file
 *
 * ImmutableString defines a storage format for strings that is designed to be
 * as compact as possible, while still being reasonably fast to decode. There
 * are two variants; one with length, and one with a “next” pointer that can
 * point to another string. As the name implies, both are designed to be
 * immutable, i.e., they are not designed to be changed (especially not in
 * length) after being encoded. See the individual classes for more details.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "securec.h"

#include <limits>
#include <string_view>

#include "my_compiler.h"

MY_COMPILER_DIAGNOSTIC_PUSH()
MY_COMPILER_GCC_DIAGNOSTIC_IGNORE("-Wsuggest-override")
MY_COMPILER_CLANG_DIAGNOSTIC_IGNORE("-Wdeprecated-dynamic-exception-spec")
MY_COMPILER_MSVC_DIAGNOSTIC_IGNORE(4251)
#include <google/protobuf/io/coded_stream.h>
MY_COMPILER_DIAGNOSTIC_POP()

#include "template_utils.h"
static constexpr int kMaxVarintBytes = 10;
/**
 * The variant with length (ImmutableStringWithLength) stores the length as a
 * Varint128 (similar to protobuf), immediately followed by the string itself.
 * (There is no zero termination.) This saves space over using e.g. a fixed
 * size_t as length, since most strings are short. This is used for keys in the
 * hash join buffer, but would be applicable other places as well.
 */
class ImmutableStringWithLength {
 public:
  ImmutableStringWithLength() = default;
  explicit ImmutableStringWithLength(const char *encoded) : m_ptr(encoded) {}
  explicit ImmutableStringWithLength(const char *encoded,
                                     bool is_pad_to_max_length)
      : m_ptr(encoded), m_pad_to_max_length(is_pad_to_max_length) {}

  inline std::string_view Decode() const;

  /// Encode the given string as an ImmutableStringWithLength, and returns
  /// a new object pointing to it. *dst must contain at least the number
  /// of bytes returned by RequiredBytesForEncode.
  ///
  /// “dst” is moved to one byte past the end of the written stream.
  static inline ImmutableStringWithLength Encode(
      const char *data, size_t length, char **dst,
      bool is_pad_to_max_length = false);

  /// Calculates an upper bound on the space required for encoding a string
  /// of the given length.
  static inline size_t RequiredBytesForEncode(size_t length) {
    return kMaxVarintBytes + length;
  }

  /// Compares full contents (data/size).
  inline bool operator==(ImmutableStringWithLength other) const;
  inline bool IsEmpty() { return m_ptr == nullptr; }
  inline const char *GetDataPointer() { return m_ptr; }

 private:
  const char *m_ptr = nullptr;
  bool m_pad_to_max_length = false;
};

// From protobuf.
inline uint64_t ZigZagEncode64(int64_t n) {
  // Note:  the right-shift must be arithmetic
  // Note:  left shift must be unsigned because of overflow
  return (static_cast<uint64_t>(n) << 1) ^ static_cast<uint64_t>(n >> 63);
}

// From protobuf.
inline int64_t ZigZagDecode64(uint64_t n) {
  // Note:  Using unsigned types prevent undefined behavior
  return static_cast<int64_t>((n >> 1) ^ (~(n & 1) + 1));
}

// Defined in sql/hash_join_buffer.cc, since that is the primary user
// of ImmutableString functionality.
std::pair<const char *, uint64_t> VarintParseSlow64(const char *p,
                                                    uint32_t res32);

// From protobuf. Included here because protobuf 3.6 does not expose the
// parse_context.h header to clients.
inline const char *VarintParse64(const char *p, uint64_t *out) {
  auto ptr = pointer_cast<const uint8_t *>(p);
  uint32_t res = ptr[0];
  if (!(res & 0x80)) {
    *out = res;
    return p + 1;
  }
  uint32_t x = ptr[1];
  res += (x - 1) << 7;
  if (!(x & 0x80)) {
    *out = res;
    return p + 2;
  }
  auto tmp = VarintParseSlow64(p, res);
  *out = tmp.second;
  return tmp.first;
}

std::string_view ImmutableStringWithLength::Decode() const {
  uint64_t size;
  if (m_ptr == nullptr) {
    return {nullptr, static_cast<size_t>(0)};
  }
  const char *data = VarintParse64(m_ptr, &size);
  if (m_pad_to_max_length) {
    // In the PTRC scenario, the header is padded with zeros to reach 10 bytes
    // (MaxVarintBytes). During decoding, we need to skip these padding zeros.
    int8_t zero_padded_size = kMaxVarintBytes - (data - m_ptr);
    data = data + zero_padded_size;
  }
  return {data, static_cast<size_t>(size)};
}

ImmutableStringWithLength ImmutableStringWithLength::Encode(
    const char *data, size_t length, char **dst, bool is_pad_to_max_length) {
  using google::protobuf::io::CodedOutputStream;

  const char *base = *dst;
  uint8_t *ptr = CodedOutputStream::WriteVarint64ToArray(
      length, pointer_cast<uint8_t *>(*dst));
  if (is_pad_to_max_length) {
    // Special handling for PTRC. Since the key and value data in the hashmap
    // and LRU reuse the same memory, we need to pad the variable-length header
    // with zeros to a fixed length(kMaxVarintBytes).
    int8_t zero_padded_size =
        kMaxVarintBytes - (pointer_cast<char *>(ptr) - base);
    memset_s(ptr, zero_padded_size, 0, zero_padded_size);
    ptr += zero_padded_size;
  }
  if (length != 0) {  // Avoid sending nullptr to memcpy().
    memcpy(ptr, data, length);
  }
  *dst = pointer_cast<char *>(ptr + length);

  return ImmutableStringWithLength(base, is_pad_to_max_length);
}

bool ImmutableStringWithLength::operator==(
    ImmutableStringWithLength other) const {
  return Decode() == other.Decode();
}

/**
 * LinkedImmutableString is designed for storing rows (values) in hash join. It
 * does not need a length, since it is implicit from the contents; however,
 * since there might be multiple values with the same key, we simulate a
 * multimap by having a “next” pointer. (Normally, linked lists are a bad idea
 * due to pointer chasing, but here, we're doing so much work for each value
 * that the overhead disappears into the noise.)
 *
 * As the next pointer is usually be very close in memory to ourselves
 * (nearly all rows are stored in the same MEM_ROOT), we don't need to store
 * the entire pointer; instead, we store the difference between the start of
 * this string and the next pointer, as a zigzag-encoded Varint128. As with
 * the length in ImmutableStringWithLength, this typically saves 6–7 bytes
 * for each value. The special value of 0 means that there is no next pointer
 * (ie., it is nullptr), as that would otherwise be an infinite loop.
 */
class LinkedImmutableString {
 public:
  struct Decoded;

  /// NOTE: nullptr is a legal value for encoded, and signals the same thing
  /// as nullptr would on a const char *.
  explicit LinkedImmutableString(const char *encoded) : m_ptr(encoded) {}

  inline Decoded Decode() const;
  inline Decoded DecodeFixed() const;

  /// Encode the given string and “next” pointer as a header for
  /// LinkedImmutableString, and returns a new object pointing to it.
  /// Note that unlike ImmutableStringWithLength::Encode(), this only
  /// encodes the header; since there is no explicitly stored length,
  /// you must write the contents of the string yourself.
  ///
  /// *dst must contain at least the number of bytes returned by
  /// RequiredBytesForEncode. It is moved to one byte past the end of the
  /// written stream (which is the right place to store the string itself).
  static inline LinkedImmutableString EncodeHeader(LinkedImmutableString next,
                                                   char **dst);

  static inline LinkedImmutableString EncodeFixedHeader(
      LinkedImmutableString next, char **dst);
  /// Calculates an upper bound on the space required for encoding a string
  /// of the given length.
  static inline size_t RequiredBytesForEncode(size_t length) {
    return kMaxVarintBytes + length;
  }

  inline bool operator==(std::nullptr_t) const { return m_ptr == nullptr; }
  inline bool operator!=(std::nullptr_t) const { return m_ptr != nullptr; }

  inline const char *GetDataPointer() { return m_ptr; }

 private:
  const char *m_ptr;
};

struct LinkedImmutableString::Decoded {
  const char *data;
  LinkedImmutableString next{nullptr};
};

LinkedImmutableString::Decoded LinkedImmutableString::Decode() const {
  LinkedImmutableString::Decoded decoded;
  uint64_t ptr_diff;
  const char *ptr = VarintParse64(m_ptr, &ptr_diff);
  decoded.data = ptr;
  if (ptr_diff == 0) {
    decoded.next = LinkedImmutableString{nullptr};
  } else {
    decoded.next = LinkedImmutableString(m_ptr + ZigZagDecode64(ptr_diff));
  }
  return decoded;
}

LinkedImmutableString::Decoded LinkedImmutableString::DecodeFixed() const {
  // In the PTRC scenario, the header is padded with zeros to reach 10 bytes
  // (MaxVarintBytes). During decoding, we need to skip these padding zeros.
  LinkedImmutableString::Decoded decoded;
  uint64_t ptr_diff;
  assert(m_ptr);
  const char *ptr = VarintParse64(m_ptr, &ptr_diff);
  int8_t zero_padded_size = kMaxVarintBytes - (ptr - m_ptr);
  decoded.data = ptr + zero_padded_size;
  if (ptr_diff == 0) {
    decoded.next = LinkedImmutableString{nullptr};
  } else {
    decoded.next = LinkedImmutableString(m_ptr + ZigZagDecode64(ptr_diff));
  }
  return decoded;
}

LinkedImmutableString LinkedImmutableString::EncodeHeader(
    LinkedImmutableString next, char **dst) {
  using google::protobuf::io::CodedOutputStream;

  const char *base = *dst;
  uint8_t *ptr = pointer_cast<uint8_t *>(*dst);
  if (next.m_ptr == nullptr) {
    *ptr++ = 0;
  } else {
    ptr = CodedOutputStream::WriteVarint64ToArray(
        ZigZagEncode64(next.m_ptr - base), ptr);
  }
  *dst = pointer_cast<char *>(ptr);
  return LinkedImmutableString(base);
}

LinkedImmutableString LinkedImmutableString::EncodeFixedHeader(
    LinkedImmutableString next, char **dst) {
  // Special handling for PTRC. Since the key and value data in the hashmap and
  // LRU reuse the same memory, we need to pad the variable-length header with
  // zeros to a fixed length(kMaxVarintBytes).
  typedef google::protobuf::io::CodedOutputStream ProtobufEncoderStream;

  const char *base = *dst;
  uint8_t *ptr = pointer_cast<uint8_t *>(*dst);
  if (next.m_ptr == nullptr) {
    *ptr++ = 0;
  } else {
    ptr = ProtobufEncoderStream::WriteVarint64ToArray(
        ZigZagEncode64(next.m_ptr - base), ptr);
  }
  int8_t zero_padded_size =
      kMaxVarintBytes - (pointer_cast<char *>(ptr) - base);
  memset_s(ptr, zero_padded_size, 0, zero_padded_size);
  ptr += zero_padded_size;
  *dst = pointer_cast<char *>(ptr);
  return LinkedImmutableString(base);
}

#endif  // IMMUTABLE_STRING_H
