/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014 Pedro Côrte-Real
    Copyright (C) 2015-2017 Roman Lebedev

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "decompressors/CrwDecompressor.h"
#include "common/Common.h"                // for uint32, ushort16, uchar8
#include "common/Point.h"                 // for iPoint2D
#include "common/RawImage.h"              // for RawImage, RawImageData
#include "decoders/RawDecoderException.h" // for RawDecoderException (ptr o...
#include "decompressors/HuffmanTable.h"   // for HuffmanTable
#include "io/BitPumpJPEG.h"               // for BitPumpJPEG, BitStream<>::...
#include "io/Buffer.h"                    // for Buffer
#include "io/ByteStream.h"                // for ByteStream
#include <algorithm>                      // for min
#include <array>                          // for array
#include <cassert>                        // for assert

using std::array;
using std::min;

namespace RawSpeed {

// The rest of this file was ported as is from dcraw.c. I don't claim to
// understand it but have tried my best to make it work safely

/*
   Construct a decode tree according the specification in *source.
   The first 16 bytes specify how many codes should be 1-bit, 2-bit
   3-bit, etc.  Bytes after that are the leaf values.

   For example, if the source is

    { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,
      0x04,0x03,0x05,0x06,0x02,0x07,0x01,0x08,0x09,0x00,0x0a,0x0b,0xff  },

   then the code is

        00                0x04
        010               0x03
        011               0x05
        100               0x06
        101               0x02
        1100              0x07
        1101              0x01
        11100             0x08
        11101             0x09
        11110             0x00
        111110            0x0a
        1111110           0x0b
        1111111           0xff
 */

HuffmanTable CrwDecompressor::makeDecoder(int n, const uchar8* source) {
  assert(n >= 0 && n <= 1);
  assert(source);

  if (n < 0 || n > 1)
    ThrowRDE("Invalid table number specified");

  HuffmanTable ht;
  auto count = ht.setNCodesPerLength(Buffer(source, 16));
  ht.setCodeValues(Buffer(source + 16, count));
  ht.setup(false, false);

  return ht;
}

array<HuffmanTable, 2> CrwDecompressor::initHuffTables(uint32 table) {
  assert(table <= 2);

  static const uchar8 first_tree[3][29] = {
      {0,    1,    4,    2,    3,    1,    2,    0,    0,    0,
       0,    0,    0,    0,    0,    0,    0x04, 0x03, 0x05, 0x06,
       0x02, 0x07, 0x01, 0x08, 0x09, 0x00, 0x0a, 0x0b, 0xff},
      {0,    2,    2,    3,    1,    1,    1,    1,    2,    0,
       0,    0,    0,    0,    0,    0,    0x03, 0x02, 0x04, 0x01,
       0x05, 0x00, 0x06, 0x07, 0x09, 0x08, 0x0a, 0x0b, 0xff},
      {0,    0,    6,    3,    1,    1,    2,    0,    0,    0,
       0,    0,    0,    0,    0,    0,    0x06, 0x05, 0x07, 0x04,
       0x08, 0x03, 0x09, 0x02, 0x00, 0x0a, 0x01, 0x0b, 0xff},
  };
  static const uchar8 second_tree[3][180] = {
      {0,    2,    2,    2,    1,    4,    2,    1,    2,    5,    1,    1,
       0,    0,    0,    139,  0x03, 0x04, 0x02, 0x05, 0x01, 0x06, 0x07, 0x08,
       0x12, 0x13, 0x11, 0x14, 0x09, 0x15, 0x22, 0x00, 0x21, 0x16, 0x0a, 0xf0,
       0x23, 0x17, 0x24, 0x31, 0x32, 0x18, 0x19, 0x33, 0x25, 0x41, 0x34, 0x42,
       0x35, 0x51, 0x36, 0x37, 0x38, 0x29, 0x79, 0x26, 0x1a, 0x39, 0x56, 0x57,
       0x28, 0x27, 0x52, 0x55, 0x58, 0x43, 0x76, 0x59, 0x77, 0x54, 0x61, 0xf9,
       0x71, 0x78, 0x75, 0x96, 0x97, 0x49, 0xb7, 0x53, 0xd7, 0x74, 0xb6, 0x98,
       0x47, 0x48, 0x95, 0x69, 0x99, 0x91, 0xfa, 0xb8, 0x68, 0xb5, 0xb9, 0xd6,
       0xf7, 0xd8, 0x67, 0x46, 0x45, 0x94, 0x89, 0xf8, 0x81, 0xd5, 0xf6, 0xb4,
       0x88, 0xb1, 0x2a, 0x44, 0x72, 0xd9, 0x87, 0x66, 0xd4, 0xf5, 0x3a, 0xa7,
       0x73, 0xa9, 0xa8, 0x86, 0x62, 0xc7, 0x65, 0xc8, 0xc9, 0xa1, 0xf4, 0xd1,
       0xe9, 0x5a, 0x92, 0x85, 0xa6, 0xe7, 0x93, 0xe8, 0xc1, 0xc6, 0x7a, 0x64,
       0xe1, 0x4a, 0x6a, 0xe6, 0xb3, 0xf1, 0xd3, 0xa5, 0x8a, 0xb2, 0x9a, 0xba,
       0x84, 0xa4, 0x63, 0xe5, 0xc5, 0xf3, 0xd2, 0xc4, 0x82, 0xaa, 0xda, 0xe4,
       0xf2, 0xca, 0x83, 0xa3, 0xa2, 0xc3, 0xea, 0xc2, 0xe2, 0xe3, 0xff, 0xff},
      {0,    2,    2,    1,    4,    1,    4,    1,    3,    3,    1,    0,
       0,    0,    0,    140,  0x02, 0x03, 0x01, 0x04, 0x05, 0x12, 0x11, 0x06,
       0x13, 0x07, 0x08, 0x14, 0x22, 0x09, 0x21, 0x00, 0x23, 0x15, 0x31, 0x32,
       0x0a, 0x16, 0xf0, 0x24, 0x33, 0x41, 0x42, 0x19, 0x17, 0x25, 0x18, 0x51,
       0x34, 0x43, 0x52, 0x29, 0x35, 0x61, 0x39, 0x71, 0x62, 0x36, 0x53, 0x26,
       0x38, 0x1a, 0x37, 0x81, 0x27, 0x91, 0x79, 0x55, 0x45, 0x28, 0x72, 0x59,
       0xa1, 0xb1, 0x44, 0x69, 0x54, 0x58, 0xd1, 0xfa, 0x57, 0xe1, 0xf1, 0xb9,
       0x49, 0x47, 0x63, 0x6a, 0xf9, 0x56, 0x46, 0xa8, 0x2a, 0x4a, 0x78, 0x99,
       0x3a, 0x75, 0x74, 0x86, 0x65, 0xc1, 0x76, 0xb6, 0x96, 0xd6, 0x89, 0x85,
       0xc9, 0xf5, 0x95, 0xb4, 0xc7, 0xf7, 0x8a, 0x97, 0xb8, 0x73, 0xb7, 0xd8,
       0xd9, 0x87, 0xa7, 0x7a, 0x48, 0x82, 0x84, 0xea, 0xf4, 0xa6, 0xc5, 0x5a,
       0x94, 0xa4, 0xc6, 0x92, 0xc3, 0x68, 0xb5, 0xc8, 0xe4, 0xe5, 0xe6, 0xe9,
       0xa2, 0xa3, 0xe3, 0xc2, 0x66, 0x67, 0x93, 0xaa, 0xd4, 0xd5, 0xe7, 0xf8,
       0x88, 0x9a, 0xd7, 0x77, 0xc4, 0x64, 0xe2, 0x98, 0xa5, 0xca, 0xda, 0xe8,
       0xf3, 0xf6, 0xa9, 0xb2, 0xb3, 0xf2, 0xd2, 0x83, 0xba, 0xd3, 0xff, 0xff},
      {0,    0,    6,    2,    1,    3,    3,    2,    5,    1,    2,    2,
       8,    10,   0,    117,  0x04, 0x05, 0x03, 0x06, 0x02, 0x07, 0x01, 0x08,
       0x09, 0x12, 0x13, 0x14, 0x11, 0x15, 0x0a, 0x16, 0x17, 0xf0, 0x00, 0x22,
       0x21, 0x18, 0x23, 0x19, 0x24, 0x32, 0x31, 0x25, 0x33, 0x38, 0x37, 0x34,
       0x35, 0x36, 0x39, 0x79, 0x57, 0x58, 0x59, 0x28, 0x56, 0x78, 0x27, 0x41,
       0x29, 0x77, 0x26, 0x42, 0x76, 0x99, 0x1a, 0x55, 0x98, 0x97, 0xf9, 0x48,
       0x54, 0x96, 0x89, 0x47, 0xb7, 0x49, 0xfa, 0x75, 0x68, 0xb6, 0x67, 0x69,
       0xb9, 0xb8, 0xd8, 0x52, 0xd7, 0x88, 0xb5, 0x74, 0x51, 0x46, 0xd9, 0xf8,
       0x3a, 0xd6, 0x87, 0x45, 0x7a, 0x95, 0xd5, 0xf6, 0x86, 0xb4, 0xa9, 0x94,
       0x53, 0x2a, 0xa8, 0x43, 0xf5, 0xf7, 0xd4, 0x66, 0xa7, 0x5a, 0x44, 0x8a,
       0xc9, 0xe8, 0xc8, 0xe7, 0x9a, 0x6a, 0x73, 0x4a, 0x61, 0xc7, 0xf4, 0xc6,
       0x65, 0xe9, 0x72, 0xe6, 0x71, 0x91, 0x93, 0xa6, 0xda, 0x92, 0x85, 0x62,
       0xf3, 0xc5, 0xb2, 0xa4, 0x84, 0xba, 0x64, 0xa5, 0xb3, 0xd2, 0x81, 0xe5,
       0xd3, 0xaa, 0xc4, 0xca, 0xf2, 0xb1, 0xe4, 0xd1, 0x83, 0x63, 0xea, 0xc3,
       0xe2, 0x82, 0xf1, 0xa3, 0xc2, 0xa1, 0xc1, 0xe3, 0xa2, 0xe1, 0xff, 0xff}};

  array<HuffmanTable, 2> mHuff = {
      {makeDecoder(0, first_tree[table]), makeDecoder(1, second_tree[table])}};

  return mHuff;
}

// FIXME: this function is horrible.
inline void
CrwDecompressor::decodeBlock(std::array<int, 64>* diffBuf,
                             const std::array<HuffmanTable, 2>& mHuff,
                             BitPumpJPEG* pump) {
  assert(diffBuf);
  assert(pump);

  // decode the block
  for (int i = 0; i < 64; i++) {
    const int leaf = mHuff[i > 0].decodeLength(*pump);
    assert(leaf >= 0);

    if (leaf == 0 && i)
      break;

    if (leaf == 0xff)
      continue;

    i += leaf >> 4;

    const int len = leaf & 15;

    if (len == 0)
      continue;

    int diff = pump->getBits(len);

    if (i >= 64)
      break;

    diff = HuffmanTable::signExtended(diff, len);

    (*diffBuf)[i] = diff;
  }
}

// FIXME: this function is horrible.
void CrwDecompressor::decompress(RawImage& mRaw, RawSpeed::Buffer* mFile,
                                 uint32 dec_table, bool lowbits) {
  assert(mFile);

  int carry = 0, base[2];

  auto mHuff = initHuffTables(dec_table);

  const uint32 height = mRaw->dim.y;
  const uint32 width = mRaw->dim.x;

  uint32 offset = 540;
  if (lowbits)
    offset += height * width / 4;

  ByteStream input(mFile, offset);
  BitPumpJPEG pump(input);

  for (uint32 j = 0; j < height;) {
    const int nBlocks = min(8u, height - j) * width >> 6;
    assert(nBlocks > 0);

    ushort16* dest = nullptr;

    uint32 i = 0;

    for (int block = 0; block < nBlocks; block++) {
      array<int, 64> diffBuf = {{}};
      decodeBlock(&diffBuf, mHuff, &pump);

      // predict and output the block

      diffBuf[0] += carry;
      carry = diffBuf[0];

      for (uint32 k = 0; k < 64; k++, i++, dest++) {
        if (i % width == 0) {
          // new line
          i = 0;

          dest = (ushort16*)mRaw->getData(0, j);

          j++;
          base[0] = base[1] = 512;
        }

        base[k & 1] += diffBuf[k];

        if (base[k & 1] >> 10)
          ThrowRDE("Error decompressing");

        assert(dest);
        *dest = base[k & 1];
      }
    }
  }

  // Add the uncompressed 2 low bits to the decoded 8 high bits
  if (lowbits) {
    offset = 26;
    ByteStream lowbitInput(mFile, offset, height * width / 4);

    for (uint32 j = 0; j < height;) {
      // Process 8 rows or however are left
      const uint32 lines = min(height - j, 8u);

      // Process 8 rows or however are left
      const uint32 nBlocks = width / 4 * lines;
      assert(nBlocks > 0);

      ushort16* dest = nullptr;

      uint32 i = 0;

      for (uint32 block = 0; block < nBlocks; block++) {
        auto c = (uint32)lowbitInput.getByte();

        // Process 8 bits in pairs
        for (uint32 r = 0; r < 8; r += 2, i++, dest++) {
          if (i % width == 0) {
            // new line
            i = 0;

            dest = (ushort16*)mRaw->getData(0, j);

            j++;
          }

          ushort16 val = (*dest << 2) | ((c >> r) & 0x0003);

          if (width == 2672 && val < 512)
            val += 2; // No idea why this is needed

          assert(dest);
          *dest = val;
        }
      }
    }
  }
}

} // namespace RawSpeed
