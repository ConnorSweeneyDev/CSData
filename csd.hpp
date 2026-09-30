// CSD 1.0.0

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

// clang-format off
// NOLINTBEGIN
#ifdef __clang__
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__) || defined(__GNUG__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wall"
  #pragma GCC diagnostic ignored "-Wextra"
  #pragma GCC diagnostic ignored "-Wpedantic"
#elif defined(_MSC_VER)
  #pragma warning(push, 0)
#endif

#ifndef STB_IMAGE_IMPLEMENTATION
  #define STB_IMAGE_IMPLEMENTATION
#endif
/* stb_image - v2.30 - public domain image loader - http://nothings.org/stb no warranty implied; use at your own risk */

namespace csd::detail
{
typedef unsigned char stbi_uc;
typedef unsigned short stbi__uint16;
typedef unsigned int stbi__uint32;
#define STBIDEF static
#define stbi_inline inline
#define STBI_ASSERT(x) ((void)0)
#define STBI_NOTUSED(v) (void)sizeof(v)
#define STBI_FREE(p) std::free(p)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) std::realloc(p, newsz)
static void *stbi__malloc(std::size_t size) { return std::malloc(size); }
static int stbi__err(const char *, const char *) { return 0; }

// public domain zlib decode    v0.2  Sean Barrett 2006-11-18
//    simple implementation
//      - all input must be provided in an upfront buffer
//      - all output is written to a single output buffer (can malloc/realloc)
//    performance
//      - fast huffman

#ifndef STBI_NO_ZLIB

// fast-way is faster to check than jpeg huffman, but slow way is slower
#define STBI__ZFAST_BITS  9 // accelerate all cases in default tables
#define STBI__ZFAST_MASK  ((1 << STBI__ZFAST_BITS) - 1)
#define STBI__ZNSYMS 288 // number of symbols in literal/length alphabet

// zlib-style huffman encoding
// (jpegs packs from left, zlib from right, so can't share code)
typedef struct
{
   stbi__uint16 fast[1 << STBI__ZFAST_BITS];
   stbi__uint16 firstcode[16];
   int maxcode[17];
   stbi__uint16 firstsymbol[16];
   stbi_uc  size[STBI__ZNSYMS];
   stbi__uint16 value[STBI__ZNSYMS];
} stbi__zhuffman;

stbi_inline static int stbi__bitreverse16(int n)
{
  n = ((n & 0xAAAA) >>  1) | ((n & 0x5555) << 1);
  n = ((n & 0xCCCC) >>  2) | ((n & 0x3333) << 2);
  n = ((n & 0xF0F0) >>  4) | ((n & 0x0F0F) << 4);
  n = ((n & 0xFF00) >>  8) | ((n & 0x00FF) << 8);
  return n;
}

stbi_inline static int stbi__bit_reverse(int v, int bits)
{
   STBI_ASSERT(bits <= 16);
   // to bit reverse n bits, reverse 16 and shift
   // e.g. 11 bits, bit reverse and shift away 5
   return stbi__bitreverse16(v) >> (16-bits);
}

static int stbi__zbuild_huffman(stbi__zhuffman *z, const stbi_uc *sizelist, int num)
{
   int i,k=0;
   int code, next_code[16], sizes[17];

   // DEFLATE spec for generating codes
   memset(sizes, 0, sizeof(sizes));
   memset(z->fast, 0, sizeof(z->fast));
   for (i=0; i < num; ++i)
      ++sizes[sizelist[i]];
   sizes[0] = 0;
   for (i=1; i < 16; ++i)
      if (sizes[i] > (1 << i))
         return stbi__err("bad sizes", "Corrupt PNG");
   code = 0;
   for (i=1; i < 16; ++i) {
      next_code[i] = code;
      z->firstcode[i] = (stbi__uint16) code;
      z->firstsymbol[i] = (stbi__uint16) k;
      code = (code + sizes[i]);
      if (sizes[i])
         if (code-1 >= (1 << i)) return stbi__err("bad codelengths","Corrupt PNG");
      z->maxcode[i] = code << (16-i); // preshift for inner loop
      code <<= 1;
      k += sizes[i];
   }
   z->maxcode[16] = 0x10000; // sentinel
   for (i=0; i < num; ++i) {
      int s = sizelist[i];
      if (s) {
         int c = next_code[s] - z->firstcode[s] + z->firstsymbol[s];
         stbi__uint16 fastv = (stbi__uint16) ((s << 9) | i);
         z->size [c] = (stbi_uc     ) s;
         z->value[c] = (stbi__uint16) i;
         if (s <= STBI__ZFAST_BITS) {
            int j = stbi__bit_reverse(next_code[s],s);
            while (j < (1 << STBI__ZFAST_BITS)) {
               z->fast[j] = fastv;
               j += (1 << s);
            }
         }
         ++next_code[s];
      }
   }
   return 1;
}

// zlib-from-memory implementation for PNG reading
//    because PNG allows splitting the zlib stream arbitrarily,
//    and it's annoying structurally to have PNG call ZLIB call PNG,
//    we require PNG read all the IDATs and combine them into a single
//    memory buffer

typedef struct
{
   stbi_uc *zbuffer, *zbuffer_end;
   int num_bits;
   int hit_zeof_once;
   stbi__uint32 code_buffer;

   char *zout;
   char *zout_start;
   char *zout_end;
   int   z_expandable;

   stbi__zhuffman z_length, z_distance;
} stbi__zbuf;

stbi_inline static int stbi__zeof(stbi__zbuf *z)
{
   return (z->zbuffer >= z->zbuffer_end);
}

stbi_inline static stbi_uc stbi__zget8(stbi__zbuf *z)
{
   return stbi__zeof(z) ? 0 : *z->zbuffer++;
}

static void stbi__fill_bits(stbi__zbuf *z)
{
   do {
      if (z->code_buffer >= (1U << z->num_bits)) {
        z->zbuffer = z->zbuffer_end;  /* treat this as EOF so we fail. */
        return;
      }
      z->code_buffer |= (unsigned int) stbi__zget8(z) << z->num_bits;
      z->num_bits += 8;
   } while (z->num_bits <= 24);
}

stbi_inline static unsigned int stbi__zreceive(stbi__zbuf *z, int n)
{
   unsigned int k;
   if (z->num_bits < n) stbi__fill_bits(z);
   k = z->code_buffer & ((1 << n) - 1);
   z->code_buffer >>= n;
   z->num_bits -= n;
   return k;
}

static int stbi__zhuffman_decode_slowpath(stbi__zbuf *a, stbi__zhuffman *z)
{
   int b,s,k;
   // not resolved by fast table, so compute it the slow way
   // use jpeg approach, which requires MSbits at top
   k = stbi__bit_reverse(a->code_buffer, 16);
   for (s=STBI__ZFAST_BITS+1; ; ++s)
      if (k < z->maxcode[s])
         break;
   if (s >= 16) return -1; // invalid code!
   // code size is s, so:
   b = (k >> (16-s)) - z->firstcode[s] + z->firstsymbol[s];
   if (b >= STBI__ZNSYMS) return -1; // some data was corrupt somewhere!
   if (z->size[b] != s) return -1;  // was originally an assert, but report failure instead.
   a->code_buffer >>= s;
   a->num_bits -= s;
   return z->value[b];
}

stbi_inline static int stbi__zhuffman_decode(stbi__zbuf *a, stbi__zhuffman *z)
{
   int b,s;
   if (a->num_bits < 16) {
      if (stbi__zeof(a)) {
         if (!a->hit_zeof_once) {
            // This is the first time we hit eof, insert 16 extra padding btis
            // to allow us to keep going; if we actually consume any of them
            // though, that is invalid data. This is caught later.
            a->hit_zeof_once = 1;
            a->num_bits += 16; // add 16 implicit zero bits
         } else {
            // We already inserted our extra 16 padding bits and are again
            // out, this stream is actually prematurely terminated.
            return -1;
         }
      } else {
         stbi__fill_bits(a);
      }
   }
   b = z->fast[a->code_buffer & STBI__ZFAST_MASK];
   if (b) {
      s = b >> 9;
      a->code_buffer >>= s;
      a->num_bits -= s;
      return b & 511;
   }
   return stbi__zhuffman_decode_slowpath(a, z);
}

static int stbi__zexpand(stbi__zbuf *z, char *zout, int n)  // need to make room for n bytes
{
   char *q;
   unsigned int cur, limit, old_limit;
   z->zout = zout;
   if (!z->z_expandable) return stbi__err("output buffer limit","Corrupt PNG");
   cur   = (unsigned int) (z->zout - z->zout_start);
   limit = old_limit = (unsigned) (z->zout_end - z->zout_start);
   if (UINT_MAX - cur < (unsigned) n) return stbi__err("outofmem", "Out of memory");
   while (cur + n > limit) {
      if(limit > UINT_MAX / 2) return stbi__err("outofmem", "Out of memory");
      limit *= 2;
   }
   q = (char *) STBI_REALLOC_SIZED(z->zout_start, old_limit, limit);
   STBI_NOTUSED(old_limit);
   if (q == NULL) return stbi__err("outofmem", "Out of memory");
   z->zout_start = q;
   z->zout       = q + cur;
   z->zout_end   = q + limit;
   return 1;
}

static const int stbi__zlength_base[31] = {
   3,4,5,6,7,8,9,10,11,13,
   15,17,19,23,27,31,35,43,51,59,
   67,83,99,115,131,163,195,227,258,0,0 };

static const int stbi__zlength_extra[31]=
{ 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0,0,0 };

static const int stbi__zdist_base[32] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577,0,0};

static const int stbi__zdist_extra[32] =
{ 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int stbi__parse_huffman_block(stbi__zbuf *a)
{
   char *zout = a->zout;
   for(;;) {
      int z = stbi__zhuffman_decode(a, &a->z_length);
      if (z < 256) {
         if (z < 0) return stbi__err("bad huffman code","Corrupt PNG"); // error in huffman codes
         if (zout >= a->zout_end) {
            if (!stbi__zexpand(a, zout, 1)) return 0;
            zout = a->zout;
         }
         *zout++ = (char) z;
      } else {
         stbi_uc *p;
         int len,dist;
         if (z == 256) {
            a->zout = zout;
            if (a->hit_zeof_once && a->num_bits < 16) {
               // The first time we hit zeof, we inserted 16 extra zero bits into our bit
               // buffer so the decoder can just do its speculative decoding. But if we
               // actually consumed any of those bits (which is the case when num_bits < 16),
               // the stream actually read past the end so it is malformed.
               return stbi__err("unexpected end","Corrupt PNG");
            }
            return 1;
         }
         if (z >= 286) return stbi__err("bad huffman code","Corrupt PNG"); // per DEFLATE, length codes 286 and 287 must not appear in compressed data
         z -= 257;
         len = stbi__zlength_base[z];
         if (stbi__zlength_extra[z]) len += stbi__zreceive(a, stbi__zlength_extra[z]);
         z = stbi__zhuffman_decode(a, &a->z_distance);
         if (z < 0 || z >= 30) return stbi__err("bad huffman code","Corrupt PNG"); // per DEFLATE, distance codes 30 and 31 must not appear in compressed data
         dist = stbi__zdist_base[z];
         if (stbi__zdist_extra[z]) dist += stbi__zreceive(a, stbi__zdist_extra[z]);
         if (zout - a->zout_start < dist) return stbi__err("bad dist","Corrupt PNG");
         if (len > a->zout_end - zout) {
            if (!stbi__zexpand(a, zout, len)) return 0;
            zout = a->zout;
         }
         p = (stbi_uc *) (zout - dist);
         if (dist == 1) { // run of one byte; common in images.
            stbi_uc v = *p;
            if (len) { do *zout++ = v; while (--len); }
         } else {
            if (len) { do *zout++ = *p++; while (--len); }
         }
      }
   }
}

static int stbi__compute_huffman_codes(stbi__zbuf *a)
{
   static const stbi_uc length_dezigzag[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
   stbi__zhuffman z_codelength;
   stbi_uc lencodes[286+32+137];//padding for maximum single op
   stbi_uc codelength_sizes[19];
   int i,n;

   int hlit  = stbi__zreceive(a,5) + 257;
   int hdist = stbi__zreceive(a,5) + 1;
   int hclen = stbi__zreceive(a,4) + 4;
   int ntot  = hlit + hdist;

   memset(codelength_sizes, 0, sizeof(codelength_sizes));
   for (i=0; i < hclen; ++i) {
      int s = stbi__zreceive(a,3);
      codelength_sizes[length_dezigzag[i]] = (stbi_uc) s;
   }
   if (!stbi__zbuild_huffman(&z_codelength, codelength_sizes, 19)) return 0;

   n = 0;
   while (n < ntot) {
      int c = stbi__zhuffman_decode(a, &z_codelength);
      if (c < 0 || c >= 19) return stbi__err("bad codelengths", "Corrupt PNG");
      if (c < 16)
         lencodes[n++] = (stbi_uc) c;
      else {
         stbi_uc fill = 0;
         if (c == 16) {
            c = stbi__zreceive(a,2)+3;
            if (n == 0) return stbi__err("bad codelengths", "Corrupt PNG");
            fill = lencodes[n-1];
         } else if (c == 17) {
            c = stbi__zreceive(a,3)+3;
         } else if (c == 18) {
            c = stbi__zreceive(a,7)+11;
         } else {
            return stbi__err("bad codelengths", "Corrupt PNG");
         }
         if (ntot - n < c) return stbi__err("bad codelengths", "Corrupt PNG");
         memset(lencodes+n, fill, c);
         n += c;
      }
   }
   if (n != ntot) return stbi__err("bad codelengths","Corrupt PNG");
   if (!stbi__zbuild_huffman(&a->z_length, lencodes, hlit)) return 0;
   if (!stbi__zbuild_huffman(&a->z_distance, lencodes+hlit, hdist)) return 0;
   return 1;
}

static int stbi__parse_uncompressed_block(stbi__zbuf *a)
{
   stbi_uc header[4];
   int len,nlen,k;
   if (a->num_bits & 7)
      stbi__zreceive(a, a->num_bits & 7); // discard
   // drain the bit-packed data into header
   k = 0;
   while (a->num_bits > 0) {
      header[k++] = (stbi_uc) (a->code_buffer & 255); // suppress MSVC run-time check
      a->code_buffer >>= 8;
      a->num_bits -= 8;
   }
   if (a->num_bits < 0) return stbi__err("zlib corrupt","Corrupt PNG");
   // now fill header the normal way
   while (k < 4)
      header[k++] = stbi__zget8(a);
   len  = header[1] * 256 + header[0];
   nlen = header[3] * 256 + header[2];
   if (nlen != (len ^ 0xffff)) return stbi__err("zlib corrupt","Corrupt PNG");
   if (a->zbuffer + len > a->zbuffer_end) return stbi__err("read past buffer","Corrupt PNG");
   if (a->zout + len > a->zout_end)
      if (!stbi__zexpand(a, a->zout, len)) return 0;
   memcpy(a->zout, a->zbuffer, len);
   a->zbuffer += len;
   a->zout += len;
   return 1;
}

static int stbi__parse_zlib_header(stbi__zbuf *a)
{
   int cmf   = stbi__zget8(a);
   int cm    = cmf & 15;
   /* int cinfo = cmf >> 4; */
   int flg   = stbi__zget8(a);
   if (stbi__zeof(a)) return stbi__err("bad zlib header","Corrupt PNG"); // zlib spec
   if ((cmf*256+flg) % 31 != 0) return stbi__err("bad zlib header","Corrupt PNG"); // zlib spec
   if (flg & 32) return stbi__err("no preset dict","Corrupt PNG"); // preset dictionary not allowed in png
   if (cm != 8) return stbi__err("bad compression","Corrupt PNG"); // DEFLATE required for png
   // window = 1 << (8 + cinfo)... but who cares, we fully buffer output
   return 1;
}

static const stbi_uc stbi__zdefault_length[STBI__ZNSYMS] =
{
   8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
   8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
   8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
   8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
   8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
   9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
   9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
   9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
   7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8
};
static const stbi_uc stbi__zdefault_distance[32] =
{
   5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
};
/*
Init algorithm:
{
   int i;   // use <= to match clearly with spec
   for (i=0; i <= 143; ++i)     stbi__zdefault_length[i]   = 8;
   for (   ; i <= 255; ++i)     stbi__zdefault_length[i]   = 9;
   for (   ; i <= 279; ++i)     stbi__zdefault_length[i]   = 7;
   for (   ; i <= 287; ++i)     stbi__zdefault_length[i]   = 8;

   for (i=0; i <=  31; ++i)     stbi__zdefault_distance[i] = 5;
}
*/

static int stbi__parse_zlib(stbi__zbuf *a, int parse_header)
{
   int final, type;
   if (parse_header)
      if (!stbi__parse_zlib_header(a)) return 0;
   a->num_bits = 0;
   a->code_buffer = 0;
   a->hit_zeof_once = 0;
   do {
      final = stbi__zreceive(a,1);
      type = stbi__zreceive(a,2);
      if (type == 0) {
         if (!stbi__parse_uncompressed_block(a)) return 0;
      } else if (type == 3) {
         return 0;
      } else {
         if (type == 1) {
            // use fixed code lengths
            if (!stbi__zbuild_huffman(&a->z_length  , stbi__zdefault_length  , STBI__ZNSYMS)) return 0;
            if (!stbi__zbuild_huffman(&a->z_distance, stbi__zdefault_distance,  32)) return 0;
         } else {
            if (!stbi__compute_huffman_codes(a)) return 0;
         }
         if (!stbi__parse_huffman_block(a)) return 0;
      }
   } while (!final);
   return 1;
}

static int stbi__do_zlib(stbi__zbuf *a, char *obuf, int olen, int exp, int parse_header)
{
   a->zout_start = obuf;
   a->zout       = obuf;
   a->zout_end   = obuf + olen;
   a->z_expandable = exp;

   return stbi__parse_zlib(a, parse_header);
}

STBIDEF char *stbi_zlib_decode_malloc_guesssize(const char *buffer, int len, int initial_size, int *outlen)
{
   stbi__zbuf a;
   char *p = (char *) stbi__malloc(initial_size);
   if (p == NULL) return NULL;
   a.zbuffer = (stbi_uc *) buffer;
   a.zbuffer_end = (stbi_uc *) buffer + len;
   if (stbi__do_zlib(&a, p, initial_size, 1, 1)) {
      if (outlen) *outlen = (int) (a.zout - a.zout_start);
      return a.zout_start;
   } else {
      STBI_FREE(a.zout_start);
      return NULL;
   }
}

STBIDEF char *stbi_zlib_decode_malloc(char const *buffer, int len, int *outlen)
{
   return stbi_zlib_decode_malloc_guesssize(buffer, len, 16384, outlen);
}

STBIDEF char *stbi_zlib_decode_malloc_guesssize_headerflag(const char *buffer, int len, int initial_size, int *outlen, int parse_header)
{
   stbi__zbuf a;
   char *p = (char *) stbi__malloc(initial_size);
   if (p == NULL) return NULL;
   a.zbuffer = (stbi_uc *) buffer;
   a.zbuffer_end = (stbi_uc *) buffer + len;
   if (stbi__do_zlib(&a, p, initial_size, 1, parse_header)) {
      if (outlen) *outlen = (int) (a.zout - a.zout_start);
      return a.zout_start;
   } else {
      STBI_FREE(a.zout_start);
      return NULL;
   }
}

STBIDEF int stbi_zlib_decode_buffer(char *obuffer, int olen, char const *ibuffer, int ilen)
{
   stbi__zbuf a;
   a.zbuffer = (stbi_uc *) ibuffer;
   a.zbuffer_end = (stbi_uc *) ibuffer + ilen;
   if (stbi__do_zlib(&a, obuffer, olen, 0, 1))
      return (int) (a.zout - a.zout_start);
   else
      return -1;
}

STBIDEF char *stbi_zlib_decode_noheader_malloc(char const *buffer, int len, int *outlen)
{
   stbi__zbuf a;
   char *p = (char *) stbi__malloc(16384);
   if (p == NULL) return NULL;
   a.zbuffer = (stbi_uc *) buffer;
   a.zbuffer_end = (stbi_uc *) buffer+len;
   if (stbi__do_zlib(&a, p, 16384, 1, 0)) {
      if (outlen) *outlen = (int) (a.zout - a.zout_start);
      return a.zout_start;
   } else {
      STBI_FREE(a.zout_start);
      return NULL;
   }
}

STBIDEF int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen)
{
   stbi__zbuf a;
   a.zbuffer = (stbi_uc *) ibuffer;
   a.zbuffer_end = (stbi_uc *) ibuffer + ilen;
   if (stbi__do_zlib(&a, obuffer, olen, 0, 0))
      return (int) (a.zout - a.zout_start);
   else
      return -1;
}
#endif

#undef STBIDEF
#undef stbi_inline
#undef STBI_ASSERT
#undef STBI_NOTUSED
#undef STBI_FREE
#undef STBI_REALLOC_SIZED
#undef STBI__ZFAST_BITS
#undef STBI__ZFAST_MASK
#undef STBI__ZNSYMS
}

#ifdef __clang__
  #pragma clang diagnostic pop
#elif defined(__GNUC__) || defined(__GNUG__)
  #pragma GCC diagnostic pop
#elif defined(_MSC_VER)
  #pragma warning(pop)
#endif
// NOLINTEND
// clang-format on

namespace csd
{
  struct aseprite
  {
    struct animation
    {
      std::string name{};
      std::pair<unsigned int, unsigned int> range{};
      std::vector<double> times{};
      std::vector<std::array<double, 2>> pivots{};
      std::vector<std::unordered_map<std::string, std::vector<std::array<double, 4>>>> hitboxes{};
    };
    struct slice
    {
      std::string name{};
      int x{};
      int y{};
      unsigned int width{};
      unsigned int height{};
    };
    struct glyph
    {
      std::uint64_t character{};
      unsigned int x{};
      unsigned int y{};
      unsigned int width{};
      unsigned int height{};
    };
    std::vector<std::byte> data{};
    unsigned int width{};
    unsigned int height{};
    unsigned int channels{};
    std::pair<unsigned int, unsigned int> resolution{};
    std::vector<animation> animations{};
    std::vector<slice> slices{};
    std::vector<glyph> glyphs{};
    bool hitboxes{};
    bool pivot{};
  };

  struct resource
  {
    std::filesystem::path file{};
    std::string name{};
    std::string space{};
    std::string pack{};
    std::vector<std::byte> blob{};
    double duration{};
    unsigned int width{};
    unsigned int height{};
    unsigned int channels{};
    unsigned int frame_width{};
    unsigned int frame_height{};
    std::vector<aseprite::animation> animations{};
    std::vector<aseprite::glyph> glyphs{};
  };
  struct placement
  {
    std::uint64_t offset{};
    std::uint64_t size{};
  };

  /**
   * The packed record shapes are written into the layout blobs and read back by CSEngine's runtime resource loaders.
   * Every field is 8 bytes wide so the structs carry no padding, and records are stored in the machine's native byte
   * order. Hitbox records store their label as a strings-blob reference in debug builds and as an FNV-1a hash of the
   * label in release builds; `hitbox_record` names the shape of the active build configuration, while the layout writer
   * spells out both shapes since it can emit either.
   */
  struct debug_hitbox_record
  {
    std::uint64_t label_offset;
    std::uint64_t label_size;
    double left, top, right, bottom;
  };
  static_assert(sizeof(debug_hitbox_record) == 48);
  struct release_hitbox_record
  {
    std::uint64_t identifier;
    double left, top, right, bottom;
  };
  static_assert(sizeof(release_hitbox_record) == 40);
#if defined(_DEBUG)
  using hitbox_record = debug_hitbox_record;
#else
  using hitbox_record = release_hitbox_record;
#endif
  struct frame_record
  {
    double left, top, right, bottom;
    double duration;
    double pivot_x, pivot_y;
    std::uint64_t hitbox_index;
    std::uint64_t hitbox_count;
  };
  static_assert(sizeof(frame_record) == 72);
  struct glyph_record
  {
    std::uint64_t character;
    double left, top, right, bottom;
    double width, height;
  };
  static_assert(sizeof(glyph_record) == 56);

  /**
   * Hashes an identifier the way release hitbox records store their labels (FNV-1a). Runtime name types should hash
   * through this function so label lookups can never drift from the packed data.
   */
  constexpr std::uint64_t hash_identifier(const std::string_view text)
  {
    std::uint64_t hash{14695981039346656037ull};
    for (const char character : text) hash = (hash ^ static_cast<std::uint64_t>(character)) * 1099511628211ull;
    return hash;
  }

  struct layout
  {
    std::string pack{};
    std::vector<std::byte> hitboxes{};
    std::vector<std::byte> frames{};
    std::vector<std::byte> glyphs{};
    std::vector<std::byte> strings{};
    std::unordered_map<std::filesystem::path, std::vector<std::pair<std::uint64_t, std::uint64_t>>> clips{};
    std::unordered_map<std::filesystem::path, std::pair<std::uint64_t, std::uint64_t>> glyph_spans{};
  };
  struct binding
  {
    std::string pack{};
    std::uint64_t signature{};
    std::unordered_map<std::filesystem::path, placement> placements{};
    placement hitboxes{};
    placement frames{};
    placement glyphs{};
    std::uint64_t strings{};
  };

  namespace detail
  {
    inline std::vector<std::byte> read_bytes(const std::filesystem::path &file)
    {
      if (!std::filesystem::exists(file)) throw std::runtime_error("File does not exist: " + file.string());
      std::ifstream input(file, std::ios::binary);
      if (!input.is_open()) throw std::runtime_error("Failed to open file: " + file.string());
      input.seekg(0, std::ios::end);
      const std::streamsize size{input.tellg()};
      input.seekg(0, std::ios::beg);
      std::vector<std::byte> container(static_cast<std::size_t>(size));
      if (size > 0 && !input.read(reinterpret_cast<char *>(container.data()), size))
        throw std::runtime_error("Failed to read file: " + file.string());
      input.close();
      return container;
    }

    inline constexpr std::string_view rpp_comment_key{"RPP_SOURCE="};

    inline std::uint32_t ogg_page_checksum(const std::span<const std::byte> page)
    {
      static const auto table{[]
                              {
                                std::array<std::uint32_t, 256> values{};
                                for (std::uint32_t index{}; index < values.size(); ++index)
                                {
                                  std::uint32_t value{index << 24u};
                                  for (unsigned int bit{}; bit < 8u; ++bit)
                                    value = (value & 0x80000000u) != 0u ? (value << 1u) ^ 0x04C11DB7u : value << 1u;
                                  values.at(index) = value;
                                }
                                return values;
                              }()};
      std::uint32_t checksum{};
      for (const auto &byte : page)
        checksum = (checksum << 8u) ^ table.at(((checksum >> 24u) ^ static_cast<std::uint32_t>(byte)) & 0xFFu);
      return checksum;
    }

    struct audio_reader
    {
      const std::vector<std::byte> &bytes;
      const std::filesystem::path &file;
      std::size_t cursor{};

      void require(const std::size_t count) const
      {
        if (cursor + count > bytes.size())
          throw std::runtime_error("Malformed audio file (unexpected end): " + file.string() + ".");
      }
      std::uint8_t byte()
      {
        require(1);
        return static_cast<std::uint8_t>(bytes.at(cursor++));
      }
      std::uint16_t word()
      {
        require(2);
        const auto value{static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes.at(cursor)) |
                                                    static_cast<std::uint16_t>(bytes.at(cursor + 1)) << 8u)};
        cursor += 2;
        return value;
      }
      std::uint32_t dword()
      {
        require(4);
        const auto value{static_cast<std::uint32_t>(bytes.at(cursor)) |
                         static_cast<std::uint32_t>(bytes.at(cursor + 1)) << 8u |
                         static_cast<std::uint32_t>(bytes.at(cursor + 2)) << 16u |
                         static_cast<std::uint32_t>(bytes.at(cursor + 3)) << 24u};
        cursor += 4;
        return value;
      }
      std::uint64_t qword()
      {
        require(8);
        std::uint64_t value{};
        for (std::size_t index{}; index < 8; ++index)
          value |= static_cast<std::uint64_t>(bytes.at(cursor + index)) << (index * 8u);
        cursor += 8;
        return value;
      }
      std::string text(const std::size_t count)
      {
        require(count);
        std::string value(reinterpret_cast<const char *>(bytes.data() + cursor), count);
        cursor += count;
        return value;
      }
      std::vector<std::byte> blob(const std::size_t count)
      {
        require(count);
        std::vector<std::byte> value(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(cursor + count));
        cursor += count;
        return value;
      }
      bool matches(const std::string_view magic) const
      {
        if (cursor + magic.size() > bytes.size()) return false;
        for (std::size_t index{}; index < magic.size(); ++index)
          if (static_cast<char>(bytes.at(cursor + index)) != magic.at(index)) return false;
        return true;
      }
      void skip(const std::size_t count)
      {
        require(count);
        cursor += count;
      }
    };

    struct ogg_page
    {
      std::size_t offset{};
      std::size_t size{};
      std::uint64_t granule{};
      std::uint32_t serial{};
      std::size_t payload_offset{};
      std::size_t payload_size{};
      std::vector<std::uint8_t> lacings{};
    };

    inline std::vector<ogg_page> ogg_pages(const std::vector<std::byte> &bytes, const std::filesystem::path &file)
    {
      std::vector<ogg_page> pages{};
      audio_reader reader{bytes, file};
      while (reader.cursor < bytes.size())
      {
        ogg_page page{};
        page.offset = reader.cursor;
        if (!reader.matches("OggS"))
          throw std::runtime_error("Malformed Ogg page in audio file: " + file.string() + ".");
        reader.skip(4);
        if (reader.byte() != 0u)
          throw std::runtime_error("Unsupported Ogg page version in audio file: " + file.string() + ".");
        reader.skip(1);
        page.granule = reader.qword();
        page.serial = reader.dword();
        reader.skip(8);
        const auto segments{reader.byte()};
        page.lacings.reserve(segments);
        for (std::uint8_t segment{}; segment < segments; ++segment)
        {
          const auto lacing{reader.byte()};
          page.lacings.push_back(lacing);
          page.payload_size += lacing;
        }
        page.payload_offset = reader.cursor;
        reader.skip(page.payload_size);
        page.size = reader.cursor - page.offset;
        pages.push_back(page);
      }
      return pages;
    }

    inline std::pair<std::vector<std::byte>, std::size_t> ogg_opus_tags(const std::vector<std::byte> &bytes,
                                                                        const std::vector<ogg_page> &pages,
                                                                        const std::filesystem::path &file)
    {
      std::vector<std::byte> tags{};
      for (std::size_t index{1}; index < pages.size(); ++index)
      {
        const auto &page{pages.at(index)};
        std::size_t position{page.payload_offset};
        for (std::size_t segment{}; segment < page.lacings.size(); ++segment)
        {
          const auto lacing{page.lacings.at(segment)};
          tags.insert(tags.end(), bytes.begin() + static_cast<std::ptrdiff_t>(position),
                      bytes.begin() + static_cast<std::ptrdiff_t>(position + lacing));
          position += lacing;
          if (lacing < 255u)
          {
            if (segment + 1 != page.lacings.size())
              throw std::runtime_error("Unsupported Ogg page layout in audio file: " + file.string() + ".");
            return {tags, index};
          }
        }
      }
      throw std::runtime_error("Unterminated Opus tags in audio file: " + file.string() + ".");
    }

    struct opus_tags
    {
      std::vector<std::byte> vendor{};
      std::vector<std::vector<std::byte>> comments{};
      std::vector<std::byte> suffix{};
    };

    inline opus_tags parse_opus_tags(const std::vector<std::byte> &tags, const std::filesystem::path &file)
    {
      audio_reader reader{tags, file};
      if (!reader.matches("OpusTags"))
        throw std::runtime_error("Missing Opus tags header in audio file: " + file.string() + ".");
      reader.skip(8);
      opus_tags result{};
      result.vendor = reader.blob(reader.dword());
      const auto count{reader.dword()};
      for (std::uint32_t index{}; index < count; ++index) result.comments.push_back(reader.blob(reader.dword()));
      result.suffix = reader.blob(tags.size() - reader.cursor);
      return result;
    }

    inline bool rpp_comment(const std::vector<std::byte> &comment)
    {
      if (comment.size() < rpp_comment_key.size()) return false;
      for (std::size_t index{}; index < rpp_comment_key.size(); ++index)
        if (static_cast<char>(std::toupper(static_cast<unsigned char>(comment.at(index)))) != rpp_comment_key.at(index))
          return false;
      return true;
    }

    inline void append_text(std::vector<std::byte> &output, const std::string_view text)
    {
      for (const auto character : text) output.push_back(static_cast<std::byte>(character));
    }
    inline void append_dword(std::vector<std::byte> &output, const std::uint32_t value)
    {
      output.push_back(static_cast<std::byte>(value & 0xFFu));
      output.push_back(static_cast<std::byte>((value >> 8u) & 0xFFu));
      output.push_back(static_cast<std::byte>((value >> 16u) & 0xFFu));
      output.push_back(static_cast<std::byte>((value >> 24u) & 0xFFu));
    }
    inline void place_dword(std::vector<std::byte> &output, const std::size_t position, const std::uint32_t value)
    {
      output.at(position) = static_cast<std::byte>(value & 0xFFu);
      output.at(position + 1) = static_cast<std::byte>((value >> 8u) & 0xFFu);
      output.at(position + 2) = static_cast<std::byte>((value >> 16u) & 0xFFu);
      output.at(position + 3) = static_cast<std::byte>((value >> 24u) & 0xFFu);
    }

    inline void append_ogg_packet(std::vector<std::byte> &output, const std::vector<std::byte> &packet,
                                  const std::uint32_t serial, std::uint32_t &sequence)
    {
      std::vector<std::uint8_t> lacings{};
      std::size_t remaining{packet.size()};
      while (remaining >= 255u)
      {
        lacings.push_back(255u);
        remaining -= 255u;
      }
      lacings.push_back(static_cast<std::uint8_t>(remaining));

      std::size_t consumed{};
      for (std::size_t first{}; first < lacings.size(); first += 255u)
      {
        const auto count{std::min<std::size_t>(255u, lacings.size() - first)};
        const auto start{output.size()};
        append_text(output, "OggS");
        output.push_back(std::byte{});
        output.push_back(static_cast<std::byte>(first == 0u ? 0x00u : 0x01u));
        for (unsigned int index{}; index < 8u; ++index) output.push_back(std::byte{});
        append_dword(output, serial);
        append_dword(output, sequence++);
        append_dword(output, 0u);
        output.push_back(static_cast<std::byte>(count));
        std::size_t payload{};
        for (std::size_t index{}; index < count; ++index)
        {
          output.push_back(static_cast<std::byte>(lacings.at(first + index)));
          payload += lacings.at(first + index);
        }
        output.insert(output.end(), packet.begin() + static_cast<std::ptrdiff_t>(consumed),
                      packet.begin() + static_cast<std::ptrdiff_t>(consumed + payload));
        consumed += payload;
        place_dword(output, start + 22u, ogg_page_checksum(std::span{output}.subspan(start)));
      }
    }

    inline void append_ogg_page(std::vector<std::byte> &output, const std::vector<std::byte> &bytes,
                                const ogg_page &page, std::uint32_t &sequence)
    {
      const auto start{output.size()};
      output.insert(output.end(), bytes.begin() + static_cast<std::ptrdiff_t>(page.offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(page.offset + page.size));
      place_dword(output, start + 18u, sequence++);
      place_dword(output, start + 22u, 0u);
      place_dword(output, start + 22u, ogg_page_checksum(std::span{output}.subspan(start)));
    }

    inline std::optional<std::vector<std::byte>> opus_extract_rpp(const std::vector<std::byte> &bytes,
                                                                  const std::vector<ogg_page> &pages,
                                                                  const std::filesystem::path &file)
    {
      const auto tags{ogg_opus_tags(bytes, pages, file).first};
      for (const auto &comment : parse_opus_tags(tags, file).comments)
        if (rpp_comment(comment))
          return std::vector<std::byte>(comment.begin() + static_cast<std::ptrdiff_t>(rpp_comment_key.size()),
                                        comment.end());
      return std::nullopt;
    }

    inline std::vector<std::byte> opus_replace_rpp(const std::vector<std::byte> &bytes,
                                                   const std::vector<ogg_page> &pages,
                                                   const std::optional<std::vector<std::byte>> &project,
                                                   const std::filesystem::path &file)
    {
      for (const auto &page : pages)
        if (page.serial != pages.front().serial)
          throw std::runtime_error("Multiplexed Ogg streams are not supported: " + file.string() + ".");

      const auto [tags, last]{ogg_opus_tags(bytes, pages, file)};
      auto parsed{parse_opus_tags(tags, file)};
      std::erase_if(parsed.comments, rpp_comment);
      if (project)
      {
        std::vector<std::byte> comment{};
        append_text(comment, rpp_comment_key);
        comment.insert(comment.end(), project->begin(), project->end());
        parsed.comments.push_back(comment);
      }

      std::vector<std::byte> packet{};
      append_text(packet, "OpusTags");
      append_dword(packet, static_cast<std::uint32_t>(parsed.vendor.size()));
      packet.insert(packet.end(), parsed.vendor.begin(), parsed.vendor.end());
      append_dword(packet, static_cast<std::uint32_t>(parsed.comments.size()));
      for (const auto &comment : parsed.comments)
      {
        append_dword(packet, static_cast<std::uint32_t>(comment.size()));
        packet.insert(packet.end(), comment.begin(), comment.end());
      }
      packet.insert(packet.end(), parsed.suffix.begin(), parsed.suffix.end());

      std::vector<std::byte> output{};
      const auto &head{pages.front()};
      output.insert(output.end(), bytes.begin() + static_cast<std::ptrdiff_t>(head.offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(head.offset + head.size));
      std::uint32_t sequence{1u};
      append_ogg_packet(output, packet, head.serial, sequence);
      for (std::size_t index{last + 1u}; index < pages.size(); ++index)
        append_ogg_page(output, bytes, pages.at(index), sequence);
      return output;
    }

    inline std::optional<std::vector<std::byte>> wav_extract_rpp(const std::vector<std::byte> &bytes,
                                                                 const std::filesystem::path &file)
    {
      audio_reader reader{bytes, file};
      reader.skip(12u);
      while (reader.cursor < bytes.size())
      {
        const auto identifier{reader.text(4u)};
        const auto size{reader.dword()};
        if (identifier == "rpp ") return reader.blob(size);
        reader.skip(std::min<std::size_t>(static_cast<std::size_t>(size) + size % 2u, bytes.size() - reader.cursor));
      }
      return std::nullopt;
    }

    inline std::vector<std::byte> wav_replace_rpp(const std::vector<std::byte> &bytes,
                                                  const std::optional<std::vector<std::byte>> &project,
                                                  const std::filesystem::path &file)
    {
      audio_reader reader{bytes, file};
      std::vector<std::byte> output{};
      output.insert(output.end(), bytes.begin(), bytes.begin() + 12);
      reader.skip(12u);
      while (reader.cursor < bytes.size())
      {
        const auto start{reader.cursor};
        const auto identifier{reader.text(4u)};
        const auto size{reader.dword()};
        reader.skip(std::min<std::size_t>(static_cast<std::size_t>(size) + size % 2u, bytes.size() - reader.cursor));
        if (identifier == "rpp ") continue;
        output.insert(output.end(), bytes.begin() + static_cast<std::ptrdiff_t>(start),
                      bytes.begin() + static_cast<std::ptrdiff_t>(reader.cursor));
      }
      if (project)
      {
        append_text(output, "rpp ");
        append_dword(output, static_cast<std::uint32_t>(project->size()));
        output.insert(output.end(), project->begin(), project->end());
        if (project->size() % 2u != 0u) output.push_back(std::byte{});
      }
      place_dword(output, 4u, static_cast<std::uint32_t>(output.size() - 8u));
      return output;
    }

    inline bool wave_audio(const std::vector<std::byte> &bytes, const std::filesystem::path &file)
    {
      audio_reader reader{bytes, file};
      if (!reader.matches("RIFF")) return false;
      reader.cursor = 8u;
      return reader.matches("WAVE");
    }

    inline double opus_duration(const std::vector<std::byte> &bytes, const std::vector<ogg_page> &pages,
                                const std::filesystem::path &file)
    {
      audio_reader head{bytes, file, pages.front().payload_offset + 10u};
      const auto pre_skip{head.word()};
      for (auto page{pages.rbegin()}; page != pages.rend(); ++page)
      {
        if (page->granule == std::numeric_limits<std::uint64_t>::max()) continue;
        if (page->granule <= pre_skip) break;
        return static_cast<double>(page->granule - pre_skip) / 48000.0;
      }
      return 0.0;
    }

    inline double wav_duration(const std::vector<std::byte> &bytes, const std::filesystem::path &file)
    {
      audio_reader reader{bytes, file};
      reader.skip(12u);
      std::uint32_t byte_rate{};
      std::uint32_t data_size{};
      while (reader.cursor + 8u <= bytes.size())
      {
        const auto identifier{reader.text(4u)};
        const auto size{reader.dword()};
        const auto padded{
          std::min<std::size_t>(static_cast<std::size_t>(size) + size % 2u, bytes.size() - reader.cursor)};
        const auto next{reader.cursor + padded};
        if (identifier == "fmt " && size >= 16u)
        {
          reader.skip(8u);
          byte_rate = reader.dword();
        }
        else if (identifier == "data")
          data_size = std::min<std::uint32_t>(size, static_cast<std::uint32_t>(bytes.size() - reader.cursor));
        reader.cursor = next;
      }
      if (byte_rate == 0u) return 0.0;
      return static_cast<double>(data_size) / static_cast<double>(byte_rate);
    }

    inline double audio_duration(const std::vector<std::byte> &bytes, const std::filesystem::path &file)
    {
      audio_reader reader{bytes, file};
      if (reader.matches("OggS"))
      {
        const auto pages{ogg_pages(bytes, file)};
        if (pages.empty() || !audio_reader{bytes, file, pages.front().payload_offset}.matches("OpusHead")) return 0.0;
        return opus_duration(bytes, pages, file);
      }
      if (wave_audio(bytes, file)) return wav_duration(bytes, file);
      return 0.0;
    }

    inline aseprite read_aseprite(const std::filesystem::path &file)
    {
      if (!std::filesystem::exists(file)) throw std::runtime_error("File does not exist: " + file.string());
      std::ifstream input(file, std::ios::binary);
      if (!input.is_open()) throw std::runtime_error("Failed to open file: " + file.string());
      input.seekg(0, std::ios::end);
      const std::streamsize total{input.tellg()};
      input.seekg(0, std::ios::beg);
      std::vector<unsigned char> bytes(static_cast<std::size_t>(total));
      if (total > 0 && !input.read(reinterpret_cast<char *>(bytes.data()), total))
        throw std::runtime_error("Failed to read file: " + file.string());
      input.close();

      struct reader_state
      {
        const std::vector<unsigned char> &bytes;
        const std::filesystem::path &file;
        std::size_t cursor{};

        void require(const std::size_t count) const
        {
          if (cursor + count > bytes.size())
            throw std::runtime_error("Malformed Aseprite file (unexpected end): " + file.string());
        }
        std::uint8_t byte()
        {
          require(1);
          return bytes.at(cursor++);
        }
        std::uint16_t word()
        {
          require(2);
          const auto value(static_cast<std::uint16_t>(static_cast<std::uint32_t>(bytes.at(cursor)) |
                                                      static_cast<std::uint32_t>(bytes.at(cursor + 1)) << 8u));
          cursor += 2;
          return value;
        }
        std::uint32_t dword()
        {
          require(4);
          const auto value(static_cast<std::uint32_t>(bytes.at(cursor)) |
                           static_cast<std::uint32_t>(bytes.at(cursor + 1)) << 8u |
                           static_cast<std::uint32_t>(bytes.at(cursor + 2)) << 16u |
                           static_cast<std::uint32_t>(bytes.at(cursor + 3)) << 24u);
          cursor += 4;
          return value;
        }
        std::int16_t integer() { return static_cast<std::int16_t>(word()); }
        std::string string()
        {
          const std::uint16_t length{word()};
          require(length);
          std::string value(reinterpret_cast<const char *>(bytes.data() + cursor), length);
          cursor += length;
          return value;
        }
        void skip(const std::size_t count)
        {
          require(count);
          cursor += count;
        }
      };
      reader_state reader{bytes, file};

      reader.dword();
      if (reader.word() != 0xA5E0) throw std::runtime_error("Not an Aseprite file: " + file.string());
      const std::uint16_t frame_count{reader.word()};
      const std::uint16_t canvas_width{reader.word()};
      const std::uint16_t canvas_height{reader.word()};
      if (reader.word() != 32) throw std::runtime_error("Aseprite file must be 32-bit RGBA: " + file.string());
      reader.cursor = 128;

      struct layer_info
      {
        std::string name{};
        std::uint16_t kind{};
        std::uint16_t blend{};
        std::uint8_t opacity{};
        bool visible{};
        bool image{};
        bool hitbox{};
        bool pivot{};
      };
      struct cel_info
      {
        std::int16_t x{}, y{};
        std::uint16_t width{}, height{};
        std::uint16_t kind{};
        std::uint16_t link{};
        std::size_t offset{}, size{};
        bool present{};
      };
      struct tag_info
      {
        std::uint16_t from{}, to{};
        std::string name{};
      };
      std::vector<layer_info> layers{};
      std::vector<std::tuple<std::uint16_t, std::uint16_t, cel_info>> raw_cels{};
      std::vector<tag_info> tags{};
      std::vector<aseprite::slice> slices{};
      std::vector<double> durations(frame_count, 0.0);
      std::string group{};
      bool image_group{};
      bool hitbox_group{};
      bool pivot_group{};

      for (std::uint16_t frame{}; frame < frame_count; ++frame)
      {
        const std::size_t frame_start{reader.cursor};
        const std::uint32_t frame_bytes{reader.dword()};
        if (reader.word() != 0xF1FA) throw std::runtime_error("Malformed Aseprite frame header: " + file.string());
        const std::uint16_t old_chunks{reader.word()};
        durations.at(frame) = static_cast<double>(reader.word()) / 1000.0;
        reader.skip(2);
        const std::uint32_t new_chunks{reader.dword()};
        const std::uint32_t chunks{new_chunks != 0 ? new_chunks : old_chunks};

        for (std::uint32_t chunk{}; chunk < chunks; ++chunk)
        {
          const std::size_t chunk_start{reader.cursor};
          const std::uint32_t chunk_size{reader.dword()};
          const std::uint16_t chunk_type{reader.word()};
          if (chunk_type == 0x2004)
          {
            const std::uint16_t flags{reader.word()};
            const std::uint16_t kind{reader.word()};
            const std::uint16_t child{reader.word()};
            reader.word();
            reader.word();
            const std::uint16_t blend{reader.word()};
            const std::uint8_t opacity{reader.byte()};
            reader.skip(3);
            const std::string name{reader.string()};
            layer_info info{};
            info.name = name;
            info.kind = kind;
            info.blend = blend;
            info.opacity = opacity;
            info.visible = (flags & 1u) != 0;
            if (child == 0)
            {
              if (kind != 1)
                throw std::runtime_error("Aseprite top-level layer '" + name +
                                         "' must be a group ('image', 'hitbox' or 'pivot'): " + file.string());
              if (name != "image" && name != "hitbox" && name != "pivot")
                throw std::runtime_error("Unexpected Aseprite top-level group '" + name +
                                         "' (only 'image', 'hitbox' and 'pivot' are allowed): " + file.string());
              group = name;
              if (name == "image") image_group = true;
              if (name == "hitbox") hitbox_group = true;
              if (name == "pivot") pivot_group = true;
            }
            else
            {
              if (group.empty())
                throw std::runtime_error("Aseprite layer '" + name + "' is outside any group: " + file.string());
              if (group == "hitbox" && kind == 1)
                throw std::runtime_error("Aseprite 'hitbox' group must be flat (no subgroups): " + file.string());
              if (group == "pivot" && kind == 1)
                throw std::runtime_error("Aseprite 'pivot' group must be flat (no subgroups): " + file.string());
              info.image = group == "image" && kind != 1;
              info.hitbox = group == "hitbox" && kind != 1;
              info.pivot = group == "pivot" && kind != 1;
            }
            layers.push_back(info);
          }
          else if (chunk_type == 0x2018)
          {
            const std::uint16_t count{reader.word()};
            reader.skip(8);
            for (std::uint16_t entry{}; entry < count; ++entry)
            {
              tag_info tag{};
              tag.from = reader.word();
              tag.to = reader.word();
              reader.byte();
              reader.word();
              reader.skip(6);
              reader.skip(3);
              reader.byte();
              tag.name = reader.string();
              tags.push_back(tag);
            }
          }
          else if (chunk_type == 0x2005)
          {
            const std::uint16_t layer{reader.word()};
            cel_info cel{};
            cel.x = reader.integer();
            cel.y = reader.integer();
            reader.byte();
            cel.kind = reader.word();
            reader.integer();
            reader.skip(5);
            if (cel.kind == 0 || cel.kind == 2)
            {
              cel.width = reader.word();
              cel.height = reader.word();
              cel.offset = reader.cursor;
              cel.size = chunk_start + chunk_size - reader.cursor;
            }
            else if (cel.kind == 1)
              cel.link = reader.word();
            else
              throw std::runtime_error("Aseprite tilemap cels are not supported: " + file.string());
            cel.present = true;
            raw_cels.emplace_back(layer, frame, cel);
          }
          else if (chunk_type == 0x2022)
          {
            const std::uint32_t keys{reader.dword()};
            reader.dword();
            reader.dword();
            const std::string name{reader.string()};
            if (keys > 0)
            {
              reader.dword();
              const auto left{static_cast<std::int32_t>(reader.dword())};
              const auto top{static_cast<std::int32_t>(reader.dword())};
              const std::uint32_t slice_width{reader.dword()};
              const std::uint32_t slice_height{reader.dword()};
              slices.push_back({name, left, top, slice_width, slice_height});
            }
          }
          reader.cursor = chunk_start + chunk_size;
        }
        reader.cursor = frame_start + frame_bytes;
      }

      if (!image_group)
        throw std::runtime_error("Aseprite file is missing the required 'image' group: " + file.string());
      if (tags.empty()) throw std::runtime_error("Aseprite file must contain at least one tag: " + file.string());
      for (std::size_t first{}; first < tags.size(); ++first)
        for (std::size_t second{first + 1}; second < tags.size(); ++second)
          if (tags.at(first).name == tags.at(second).name)
            throw std::runtime_error("Duplicate Aseprite tag name '" + tags.at(first).name + "': " + file.string());
      for (std::size_t first{}; first < layers.size(); ++first)
        if (layers.at(first).hitbox)
          for (std::size_t second{first + 1}; second < layers.size(); ++second)
            if (layers.at(second).hitbox && layers.at(first).name == layers.at(second).name)
              throw std::runtime_error("Duplicate Aseprite hitbox layer '" + layers.at(first).name +
                                       "': " + file.string());
      if (std::none_of(layers.begin(), layers.end(), [](const layer_info &layer) { return layer.image; }))
        throw std::runtime_error("Aseprite 'image' group has no layers: " + file.string());
      std::size_t pivot_layer{layers.size()};
      if (pivot_group)
      {
        std::size_t pivot_layers{};
        for (std::size_t index{}; index < layers.size(); ++index)
          if (layers.at(index).pivot)
          {
            pivot_layer = index;
            ++pivot_layers;
          }
        if (pivot_layers != 1)
          throw std::runtime_error("Aseprite 'pivot' group must contain exactly one layer: " + file.string());
      }

      std::vector<cel_info> grid(static_cast<std::size_t>(layers.size()) * frame_count);
      for (const auto &[layer, frame, cel] : raw_cels)
        if (layer < layers.size() && frame < frame_count)
          grid.at((static_cast<std::size_t>(layer) * frame_count) + frame) = cel;
      const auto resolve{[&](std::size_t layer, std::uint16_t frame) -> const cel_info *
                         {
                           const cel_info &cel{grid.at((layer * frame_count) + frame)};
                           if (!cel.present) return nullptr;
                           if (cel.kind != 1) return &cel;
                           if (cel.link >= frame_count) return nullptr;
                           const cel_info &linked{grid.at((layer * frame_count) + cel.link)};
                           return linked.present ? &linked : nullptr;
                         }};
      const auto decode{[&](const cel_info &cel) -> std::vector<unsigned char>
                        {
                          std::vector<unsigned char> pixels(static_cast<std::size_t>(cel.width) * cel.height * 4);
                          if (cel.kind == 0)
                          {
                            if (cel.size < pixels.size())
                              throw std::runtime_error("Truncated raw cel in Aseprite file: " + file.string());
                            std::copy_n(bytes.data() + cel.offset, pixels.size(), pixels.data());
                          }
                          else
                          {
                            const int decoded{stbi_zlib_decode_buffer(
                              reinterpret_cast<char *>(pixels.data()), static_cast<int>(pixels.size()),
                              reinterpret_cast<const char *>(bytes.data() + cel.offset), static_cast<int>(cel.size))};
                            if (decoded < 0 || static_cast<std::size_t>(decoded) != pixels.size())
                              throw std::runtime_error("Failed to decompress Aseprite cel: " + file.string());
                          }
                          return pixels;
                        }};
      const auto decompose{
        [&](const cel_info &cel, const std::vector<unsigned char> &pixels) -> std::vector<std::array<double, 4>>
        {
          const int width{static_cast<int>(cel.width)};
          const int height{static_cast<int>(cel.height)};
          std::vector<unsigned char> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
          for (int pixel{}; pixel < width * height; ++pixel)
            mask.at(static_cast<std::size_t>(pixel)) =
              pixels.at((static_cast<std::size_t>(pixel) * 4) + 3) != 0 ? 1 : 0;
          struct solidity
          {
            const std::vector<unsigned char> &mask;
            int width{}, height{};

            bool operator()(const int left, const int top) const
            {
              return left >= 0 && top >= 0 && left < width && top < height &&
                     mask.at((static_cast<std::size_t>(top) * static_cast<std::size_t>(width)) +
                             static_cast<std::size_t>(left)) != 0;
            }
          };
          const solidity solid{mask, width, height};

          struct corner
          {
            int x, y;
            bool up;
          };
          const int lattice_height{height + 1};
          std::vector<int> here(static_cast<std::size_t>(width + 1) * static_cast<std::size_t>(lattice_height), -1);
          std::vector<corner> corners{};
          for (int top{}; top <= height; ++top)
            for (int left{}; left <= width; ++left)
            {
              const bool top_left{solid(left - 1, top - 1)}, top_right{solid(left, top - 1)},
                bottom_left{solid(left - 1, top)}, bottom_right{solid(left, top)};
              if (top_left + top_right + bottom_left + bottom_right != 3) continue;
              here.at((static_cast<std::size_t>(left) * static_cast<std::size_t>(lattice_height)) +
                      static_cast<std::size_t>(top)) = static_cast<int>(corners.size());
              corners.push_back({left, top, !bottom_left || !bottom_right});
            }

          struct chord { int line, lo, hi, a, b; };
          std::vector<chord> horizontal{}, vertical{};
          for (int top{}; top <= height; ++top)
            for (int left{}; left < width;)
            {
              if (!(solid(left, top - 1) && solid(left, top)))
              {
                ++left;
                continue;
              }
              const int low{left};
              while (left < width && solid(left, top - 1) && solid(left, top)) ++left;
              const int first{here.at((static_cast<std::size_t>(low) * static_cast<std::size_t>(lattice_height)) +
                                      static_cast<std::size_t>(top))};
              const int second{here.at((static_cast<std::size_t>(left) * static_cast<std::size_t>(lattice_height)) +
                                       static_cast<std::size_t>(top))};
              if (first >= 0 && second >= 0) horizontal.push_back({top, low, left, first, second});
            }
          for (int left{}; left <= width; ++left)
            for (int top{}; top < height;)
            {
              if (!(solid(left - 1, top) && solid(left, top)))
              {
                ++top;
                continue;
              }
              const int low{top};
              while (top < height && solid(left - 1, top) && solid(left, top)) ++top;
              const int first{here.at((static_cast<std::size_t>(left) * static_cast<std::size_t>(lattice_height)) +
                                      static_cast<std::size_t>(low))};
              const int second{here.at((static_cast<std::size_t>(left) * static_cast<std::size_t>(lattice_height)) +
                                       static_cast<std::size_t>(top))};
              if (first >= 0 && second >= 0) vertical.push_back({left, low, top, first, second});
            }

          const int num_horizontal{static_cast<int>(horizontal.size())},
            num_vertical{static_cast<int>(vertical.size())};
          std::vector<std::vector<int>> conflicts(static_cast<std::size_t>(num_horizontal));
          for (int i{}; i < num_horizontal; ++i)
            for (int j{}; j < num_vertical; ++j)
            {
              const int first{horizontal.at(static_cast<std::size_t>(i)).lo},
                second{horizontal.at(static_cast<std::size_t>(i)).hi};
              const int third{horizontal.at(static_cast<std::size_t>(i)).line};
              const int fourth{vertical.at(static_cast<std::size_t>(j)).line};
              const int fifth{vertical.at(static_cast<std::size_t>(j)).lo},
                sixth{vertical.at(static_cast<std::size_t>(j)).hi};
              const bool cross{first < fourth && fourth < second && fifth < third && third < sixth};
              const bool share{(fourth == first || fourth == second) && (third == fifth || third == sixth)};
              if (cross || share) conflicts.at(static_cast<std::size_t>(i)).push_back(j);
            }

          std::vector<int> match_h(static_cast<std::size_t>(num_horizontal), -1),
            match_v(static_cast<std::size_t>(num_vertical), -1);
          std::vector<unsigned char> seen(static_cast<std::size_t>(num_vertical));
          const std::function<bool(int)> augment{[&](int across) -> bool
                                                 {
                                                   for (const int up : conflicts.at(static_cast<std::size_t>(across)))
                                                   {
                                                     if (seen.at(static_cast<std::size_t>(up))) continue;
                                                     seen.at(static_cast<std::size_t>(up)) = 1;
                                                     if (match_v.at(static_cast<std::size_t>(up)) < 0 ||
                                                         augment(match_v.at(static_cast<std::size_t>(up))))
                                                     {
                                                       match_v.at(static_cast<std::size_t>(up)) = across;
                                                       match_h.at(static_cast<std::size_t>(across)) = up;
                                                       return true;
                                                     }
                                                   }
                                                   return false;
                                                 }};
          for (int across{}; across < num_horizontal; ++across)
          {
            std::ranges::fill(seen, static_cast<unsigned char>(0));
            augment(across);
          }
          std::vector<unsigned char> visited_h(static_cast<std::size_t>(num_horizontal)),
            visited_v(static_cast<std::size_t>(num_vertical));
          const std::function<void(int)> mark{[&](int across)
                                              {
                                                visited_h.at(static_cast<std::size_t>(across)) = 1;
                                                for (const int up : conflicts.at(static_cast<std::size_t>(across)))
                                                {
                                                  if (visited_v.at(static_cast<std::size_t>(up))) continue;
                                                  visited_v.at(static_cast<std::size_t>(up)) = 1;
                                                  const int next{match_v.at(static_cast<std::size_t>(up))};
                                                  if (next >= 0 && !visited_h.at(static_cast<std::size_t>(next)))
                                                    mark(next);
                                                }
                                              }};
          for (int across{}; across < num_horizontal; ++across)
            if (match_h.at(static_cast<std::size_t>(across)) < 0 && !visited_h.at(static_cast<std::size_t>(across)))
              mark(across);

          std::vector<unsigned char> vertical_wall(
            static_cast<std::size_t>(width + 1) * static_cast<std::size_t>(height), 0);
          std::vector<unsigned char> horizontal_wall(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height + 1), 0);
          const auto vwall{[&](int left, int top) -> unsigned char &
                           {
                             return vertical_wall.at(
                               (static_cast<std::size_t>(left) * static_cast<std::size_t>(height)) +
                               static_cast<std::size_t>(top));
                           }};
          const auto hwall{[&](int left, int top) -> unsigned char &
                           {
                             return horizontal_wall.at(
                               (static_cast<std::size_t>(left) * static_cast<std::size_t>(height + 1)) +
                               static_cast<std::size_t>(top));
                           }};
          std::vector<unsigned char> resolved(corners.size(), 0);
          for (int i{}; i < num_horizontal; ++i)
            if (visited_h.at(static_cast<std::size_t>(i)))
            {
              const chord &current{horizontal.at(static_cast<std::size_t>(i))};
              for (int mid{current.lo}; mid < current.hi; ++mid) hwall(mid, current.line) = 1;
              resolved.at(static_cast<std::size_t>(current.a)) = resolved.at(static_cast<std::size_t>(current.b)) = 1;
            }
          for (int j{}; j < num_vertical; ++j)
            if (!visited_v.at(static_cast<std::size_t>(j)))
            {
              const chord &current{vertical.at(static_cast<std::size_t>(j))};
              for (int mid{current.lo}; mid < current.hi; ++mid) vwall(current.line, mid) = 1;
              resolved.at(static_cast<std::size_t>(current.a)) = resolved.at(static_cast<std::size_t>(current.b)) = 1;
            }
          for (std::size_t k{}; k < corners.size(); ++k)
          {
            if (resolved.at(k)) continue;
            const int left{corners.at(k).x};
            if (corners.at(k).up)
              for (int top{corners.at(k).y - 1}; top >= 0; --top)
              {
                if (!solid(left - 1, top) || !solid(left, top) || vwall(left, top)) break;
                vwall(left, top) = 1;
                if (hwall(left - 1, top) || hwall(left, top)) break;
              }
            else
              for (int top{corners.at(k).y}; top < height; ++top)
              {
                if (!solid(left - 1, top) || !solid(left, top) || vwall(left, top)) break;
                vwall(left, top) = 1;
                if (hwall(left - 1, top + 1) || hwall(left, top + 1)) break;
              }
          }

          std::vector<unsigned char> covered(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
          const auto cover{[&](int left, int top) -> unsigned char &
                           {
                             return covered.at((static_cast<std::size_t>(top) * static_cast<std::size_t>(width)) +
                                               static_cast<std::size_t>(left));
                           }};
          std::vector<std::array<double, 4>> rectangles{};
          for (int row{}; row < height; ++row)
            for (int column{}; column < width; ++column)
            {
              if (!solid(column, row) || cover(column, row)) continue;
              int right{column + 1};
              while (right < width && solid(right, row) && !cover(right, row) && !vwall(right, row)) ++right;
              int bottom{row + 1};
              while (bottom < height)
              {
                bool fine{true};
                for (int current{column}; current < right && fine; ++current)
                  if (!solid(current, bottom) || cover(current, bottom) || hwall(current, bottom)) fine = false;
                for (int current{column + 1}; current < right && fine; ++current)
                  if (vwall(current, bottom)) fine = false;
                if (!fine) break;
                ++bottom;
              }
              for (int top{row}; top < bottom; ++top)
                for (int left{column}; left < right; ++left) cover(left, top) = 1;
              rectangles.push_back(
                {static_cast<double>(cel.x + column), static_cast<double>(canvas_height - (cel.y + row)),
                 static_cast<double>(cel.x + right), static_cast<double>(canvas_height - (cel.y + bottom))});
            }
          return rectangles;
        }};

      const unsigned int frame_width{canvas_width}, frame_height{canvas_height};
      const unsigned int width{frame_width * frame_count}, height{frame_height};
      std::vector<unsigned char> sheet(static_cast<std::size_t>(width) * height * 4, 0);
      for (std::uint16_t frame{}; frame < frame_count; ++frame)
        for (std::size_t layer{}; layer < layers.size(); ++layer)
        {
          if (!layers.at(layer).image || !layers.at(layer).visible) continue;
          if (layers.at(layer).blend != 0)
            throw std::runtime_error("Unsupported Aseprite blend mode on layer '" + layers.at(layer).name +
                                     "' (only Normal is supported): " + file.string());
          const cel_info *cel{resolve(layer, frame)};
          if (!cel || cel->width == 0 || cel->height == 0) continue;
          const std::vector<unsigned char> pixels{decode(*cel)};
          const double layer_alpha{layers.at(layer).opacity / 255.0};
          for (unsigned int row{}; row < cel->height; ++row)
          {
            const int top{cel->y + static_cast<int>(row)};
            if (top < 0 || std::cmp_greater_equal(top, static_cast<int>(frame_height))) continue;
            for (unsigned int column{}; column < cel->width; ++column)
            {
              const int left{cel->x + static_cast<int>(column)};
              if (left < 0 || std::cmp_greater_equal(left, static_cast<int>(frame_width))) continue;
              const std::size_t source{((static_cast<std::size_t>(row) * cel->width) + column) * 4};
              const double source_alpha{pixels.at(source + 3) / 255.0 * layer_alpha};
              if (source_alpha <= 0.0) continue;
              const std::size_t destination{
                ((static_cast<std::size_t>(top) * width) +
                 ((static_cast<std::size_t>(frame) * frame_width) + static_cast<std::size_t>(left))) *
                4};
              const double destination_alpha{sheet.at(destination + 3) / 255.0};
              const double alpha{source_alpha + (destination_alpha * (1.0 - source_alpha))};
              if (alpha <= 0.0) continue;
              for (std::size_t channel{}; channel < 3; ++channel)
              {
                const double source_channel{pixels.at(source + channel) / 255.0};
                const double destination_channel{sheet.at(destination + channel) / 255.0};
                const double blended{
                  ((source_channel * source_alpha) + (destination_channel * destination_alpha * (1.0 - source_alpha))) /
                  alpha};
                sheet.at(destination + channel) = static_cast<unsigned char>(std::lround(blended * 255.0));
              }
              sheet.at(destination + 3) = static_cast<unsigned char>(std::lround(alpha * 255.0));
            }
          }
        }
      for (unsigned int row{}; row < height / 2; ++row)
        std::swap_ranges(sheet.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * width * 4),
                         sheet.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(row) + 1) * width * 4),
                         sheet.begin() +
                           static_cast<std::ptrdiff_t>(static_cast<std::size_t>(height - 1 - row) * width * 4));

      std::vector<std::array<double, 2>> pivots{};
      if (pivot_group)
      {
        pivots.resize(frame_count);
        for (std::uint16_t frame{}; frame < frame_count; ++frame)
        {
          const cel_info *cel{resolve(pivot_layer, frame)};
          if (!cel || cel->width == 0 || cel->height == 0)
            throw std::runtime_error("Aseprite 'pivot' layer must contain exactly one pixel on every frame: " +
                                     file.string());
          const std::vector<unsigned char> pixels{decode(*cel)};
          std::size_t found{};
          int pivot_x{}, pivot_y{};
          for (unsigned int row{}; row < cel->height; ++row)
            for (unsigned int column{}; column < cel->width; ++column)
              if (pixels.at((((static_cast<std::size_t>(row) * cel->width) + column) * 4) + 3) != 0)
              {
                pivot_x = cel->x + static_cast<int>(column);
                pivot_y = cel->y + static_cast<int>(row);
                ++found;
              }
          if (found != 1)
            throw std::runtime_error("Aseprite 'pivot' layer must contain exactly one pixel on every frame: " +
                                     file.string());
          if (pivot_x < 0 || pivot_y < 0 || std::cmp_greater_equal(pivot_x, frame_width) ||
              std::cmp_greater_equal(pivot_y, frame_height))
            throw std::runtime_error("Aseprite pivot pixel must be within the canvas: " + file.string());
          pivots.at(frame) = {static_cast<double>(pivot_x),
                              static_cast<double>(frame_height) - 1.0 - static_cast<double>(pivot_y)};
        }
      }

      aseprite result{};
      result.data.assign(reinterpret_cast<std::byte *>(sheet.data()),
                         reinterpret_cast<std::byte *>(sheet.data()) + sheet.size());
      result.width = width;
      result.height = height;
      result.channels = 4;
      result.resolution = {frame_width, frame_height};
      result.slices = std::move(slices);
      result.hitboxes = hitbox_group;
      result.pivot = pivot_group;
      if (!result.slices.empty())
      {
        const auto codepoint{[&file](const std::string &name) -> std::uint64_t
                             {
                               const auto invalid{
                                 [&file, &name]()
                                 {
                                   return std::runtime_error("Aseprite slice name '" + name +
                                                             "' must be a single character: " + file.string());
                                 }};
                               if (name.empty()) throw invalid();
                               const auto first{static_cast<unsigned char>(name.at(0))};
                               std::size_t length{1};
                               std::uint64_t character{first};
                               if (first >= 0xF0)
                                 length = 4, character = first & 0x07u;
                               else if (first >= 0xE0)
                                 length = 3, character = first & 0x0Fu;
                               else if (first >= 0xC0)
                                 length = 2, character = first & 0x1Fu;
                               else if (first >= 0x80)
                                 throw invalid();
                               if (name.size() != length) throw invalid();
                               for (std::size_t offset{1}; offset < length; ++offset)
                               {
                                 const auto continuation{static_cast<unsigned char>(name.at(offset))};
                                 if ((continuation & 0xC0u) != 0x80u) throw invalid();
                                 character = (character << 6u) | (continuation & 0x3Fu);
                               }
                               return character;
                             }};
        for (const aseprite::slice &entry : result.slices)
        {
          if (entry.x < 0 || entry.y < 0 || entry.width == 0 || entry.height == 0 ||
              static_cast<unsigned int>(entry.x) + entry.width > frame_width ||
              static_cast<unsigned int>(entry.y) + entry.height > frame_height)
            throw std::runtime_error("Aseprite slice '" + entry.name +
                                     "' must fit within the canvas: " + file.string());
          if (entry.height != result.slices.front().height)
            throw std::runtime_error("Aseprite slice '" + entry.name +
                                     "' must match the height of every other slice: " + file.string());
          result.glyphs.push_back({codepoint(entry.name), static_cast<unsigned int>(entry.x),
                                   static_cast<unsigned int>(entry.y), entry.width, entry.height});
        }
        std::sort(result.glyphs.begin(), result.glyphs.end(),
                  [](const aseprite::glyph &left, const aseprite::glyph &right)
                  { return left.character < right.character; });
        for (std::size_t index{}; index + 1 < result.glyphs.size(); ++index)
          if (result.glyphs.at(index).character == result.glyphs.at(index + 1).character)
            throw std::runtime_error("Aseprite file contains duplicate slices for the same character: " +
                                     file.string());
      }
      for (const tag_info &tag : tags)
      {
        if (tag.from >= frame_count || tag.to >= frame_count || tag.from > tag.to)
          throw std::runtime_error("Aseprite tag '" + tag.name + "' has an invalid frame range: " + file.string());
        aseprite::animation animation{};
        animation.name = tag.name;
        animation.range = {tag.from, tag.to};
        for (std::uint16_t frame{tag.from}; frame <= tag.to; ++frame)
        {
          animation.times.push_back(durations.at(frame));
          if (pivot_group) animation.pivots.push_back(pivots.at(frame));
          std::unordered_map<std::string, std::vector<std::array<double, 4>>> hitboxes{};
          for (std::size_t layer{}; layer < layers.size(); ++layer)
          {
            if (!layers.at(layer).hitbox || !layers.at(layer).visible) continue;
            const cel_info *cel{resolve(layer, frame)};
            if (!cel || cel->width == 0 || cel->height == 0) continue;
            std::vector<std::array<double, 4>> rectangles{decompose(*cel, decode(*cel))};
            if (!rectangles.empty()) hitboxes.insert_or_assign(layers.at(layer).name, std::move(rectangles));
          }
          animation.hitboxes.push_back(std::move(hitboxes));
        }
        result.animations.push_back(std::move(animation));
      }
      return result;
    }

    inline std::vector<std::string> hitbox_names(const resource &item)
    {
      std::vector<std::string> names{};
      for (const auto &animation : item.animations)
        for (const auto &frame : animation.hitboxes)
          for (const auto &[identifier, rectangles] : frame)
            if (std::ranges::find(names, identifier) == names.end()) names.push_back(identifier);
      return names;
    }
  }

  /**
   * Extension predicates for packable resource files; extensions are expected in lowercase.
   *
   * Textures and fonts are aseprite files; sounds and musics are opus or wav files. Anything else found by a directory
   * glob is not a packable resource (for example the .rpp Reaper project files that sit next to their rendered audio).
   */
  inline bool packable_texture(const std::string &extension) { return extension == ".aseprite"; }
  inline bool packable_audio(const std::string &extension) { return extension == ".opus" || extension == ".wav"; }

  /**
   * Extracts the Reaper project embedded in an opus/wav audio file, or nothing if the file carries none.
   *
   * Opus files store the project in an "RPP_SOURCE" comment tag and wav files store it in an "rpp " chunk; both are
   * ignored by audio decoders. Files that are not Opus or WAV yield nothing.
   */
  inline std::optional<std::vector<std::byte>> audio_extract_rpp(const std::vector<std::byte> &bytes,
                                                                 const std::filesystem::path &file)
  {
    detail::audio_reader reader{bytes, file};
    if (reader.matches("OggS"))
    {
      const auto pages{detail::ogg_pages(bytes, file)};
      if (pages.empty() || !detail::audio_reader{bytes, file, pages.front().payload_offset}.matches("OpusHead"))
        return std::nullopt;
      return opus_extract_rpp(bytes, pages, file);
    }
    if (detail::wave_audio(bytes, file)) return detail::wav_extract_rpp(bytes, file);
    return std::nullopt;
  }

  /**
   * Replaces the Reaper project embedded in an opus/wav audio file, embedding the given project or stripping any
   * embedded project when none is given. Returns the rewritten audio file.
   */
  inline std::vector<std::byte> audio_replace_rpp(const std::vector<std::byte> &bytes,
                                                  const std::optional<std::vector<std::byte>> &project,
                                                  const std::filesystem::path &file)
  {
    detail::audio_reader reader{bytes, file};
    if (reader.matches("OggS"))
    {
      const auto pages{detail::ogg_pages(bytes, file)};
      if (pages.empty() || !detail::audio_reader{bytes, file, pages.front().payload_offset}.matches("OpusHead"))
        throw std::runtime_error("Ogg audio file is not an Opus stream: " + file.string() + ".");
      return opus_replace_rpp(bytes, pages, project, file);
    }
    if (detail::wave_audio(bytes, file)) return detail::wav_replace_rpp(bytes, project, file);
    throw std::runtime_error("Audio file must be Opus or WAV to carry a Reaper project: " + file.string() + ".");
  }

  /**
   * Loads a resource file into its parsed, packable form.
   *
   * Textures and fonts are parsed as aseprite files and validated against the engine's conventions: textures require a
   * 'pivot' group, fonts must not contain 'hitbox' or 'pivot' groups and need at least one slice. Sounds and musics are
   * read as raw audio data with any embedded Reaper project stripped, and their playing time is measured from the
   * stripped data; audio whose playing time cannot be measured is rejected, since the engine times playback against it.
   *
   * This function's parameters behave as follows:
   * | `file`: The resource file to load.
   * | `name`: The accessor name of the resource (usually the file stem).
   * | `space`: The resource kind: "image", "font", "sound" or "music".
   * | `pack`: The name of the csp pack the resource belongs to.
   */
  inline resource load(const std::filesystem::path &file, const std::string &name, const std::string &space,
                       const std::string &pack)
  {
    resource current{};
    current.file = file;
    current.name = name;
    current.space = space;
    current.pack = pack;
    if (space == "image" || space == "font")
    {
      auto texture{detail::read_aseprite(file)};
      if (space == "image" && !texture.pivot)
        throw std::runtime_error("Texture is missing the required 'pivot' group: " + file.string());
      if (space == "font")
      {
        if (texture.hitboxes) throw std::runtime_error("Font must not contain a 'hitbox' group: " + file.string());
        if (texture.pivot) throw std::runtime_error("Font must not contain a 'pivot' group: " + file.string());
        if (texture.glyphs.empty()) throw std::runtime_error("Font must contain at least one slice: " + file.string());
        current.glyphs = std::move(texture.glyphs);
      }
      current.blob = std::move(texture.data);
      current.width = texture.width;
      current.height = texture.height;
      current.channels = texture.channels;
      current.frame_width = texture.resolution.first;
      current.frame_height = texture.resolution.second;
      current.animations = std::move(texture.animations);
    }
    else if (space == "sound" || space == "music")
    {
      current.blob = detail::read_bytes(file);
      if (audio_extract_rpp(current.blob, file)) current.blob = audio_replace_rpp(current.blob, std::nullopt, file);
      current.duration = detail::audio_duration(current.blob, file);
      if (current.duration <= 0.0)
        throw std::runtime_error("Could not measure the playing time of audio file (it must be a well-formed Opus or "
                                 "WAV file with a non-empty stream): " +
                                 file.string() + ".");
    }
    else
      throw std::runtime_error("Unknown resource space '" + space + "' for file: " + file.string() + ".");
    return current;
  }

  /**
   * Returns the preamble of the generated accessor header and source files.
   */
  inline std::string header_preamble()
  {
    return "// This file is automatically generated, do not edit manually.\n\n"
           "#pragma once\n\n"
           "#include \"cse/resource.hpp\"\n\n";
  }
  inline std::string source_preamble(const std::string &header)
  {
    return std::format("// This file is automatically generated, do not edit manually.\n\n"
                       "#include \"{}\"\n\n"
                       "#include \"cse/resource.hpp\"\n\n",
                       header);
  }

  /**
   * Computes the binary hitbox/frame/glyph/string layout of every pack, one layout per pack in sorted pack order.
   *
   * The blobs are sequences of the record structs above, appended to each pack's csp container after the resource
   * blobs; the returned clips and glyph spans index into them and are consumed by accessor_source. In debug, hitbox
   * labels are stored in the strings blob and referenced by offset; in release they are stored as FNV-1a hashes.
   */
  inline std::vector<layout> layouts(const std::vector<const resource *> &resources, const bool debug)
  {
    const auto put{[](std::vector<std::byte> &out, const auto &record)
                   {
                     const auto *raw{reinterpret_cast<const std::byte *>(&record)};
                     out.insert(out.end(), raw, raw + sizeof(record));
                   }};

    std::vector<std::string> packs{};
    for (const auto *item : resources)
      if (std::ranges::find(packs, item->pack) == packs.end()) packs.push_back(item->pack);
    std::ranges::sort(packs);

    std::vector<layout> result{};
    result.reserve(packs.size());
    for (const std::string &current_pack : packs)
    {
      layout current{};
      current.pack = current_pack;
      std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> string_pool{};
      std::uint64_t hitboxes_total{};
      std::uint64_t frames_total{};
      std::uint64_t glyphs_total{};
      for (const auto *item : resources)
      {
        if ((item->space != "image" && item->space != "font") || item->pack != current_pack) continue;
        const unsigned int per_row{item->width / item->frame_width};
        const unsigned int per_column{item->height / item->frame_height};
        for (const auto &animation : item->animations)
        {
          const std::uint64_t frame_index{frames_total};
          const auto start{animation.range.first};
          const auto end{animation.range.second};
          std::size_t index{};
          for (unsigned int frame{start}; frame <= end; ++frame)
          {
            const unsigned int first{frame % per_row};
            const unsigned int second{(per_column - 1) - (frame / per_row)};
            const double top{static_cast<double>((second + 1) * item->frame_height) /
                             static_cast<double>(item->height)};
            const double left{static_cast<double>(first * item->frame_width) / static_cast<double>(item->width)};
            const double bottom{static_cast<double>(second * item->frame_height) / static_cast<double>(item->height)};
            const double right{static_cast<double>((first + 1) * item->frame_width) / static_cast<double>(item->width)};

            const std::uint64_t hitbox_index{hitboxes_total};
            std::uint64_t hitbox_count{};
            if (index < animation.hitboxes.size() && !animation.hitboxes.at(index).empty())
              for (const auto &[identifier, rectangles] : animation.hitboxes.at(index))
              {
                const auto full{std::format("{}.{}", item->name, identifier)};
                std::uint64_t label_offset{}, label_size{}, label_hash{};
                if (debug)
                {
                  auto entry{string_pool.find(full)};
                  if (entry == string_pool.end())
                  {
                    const auto offset{static_cast<std::uint64_t>(current.strings.size())};
                    const auto *raw{reinterpret_cast<const std::byte *>(full.data())};
                    current.strings.insert(current.strings.end(), raw, raw + full.size());
                    entry = string_pool.emplace(full, std::pair{offset, static_cast<std::uint64_t>(full.size())}).first;
                  }
                  label_offset = entry->second.first;
                  label_size = entry->second.second;
                }
                else
                  label_hash = hash_identifier(full);
                for (const auto &bounds : rectangles)
                {
                  if (debug)
                    put(current.hitboxes, debug_hitbox_record{label_offset, label_size, bounds.at(0), bounds.at(1),
                                                              bounds.at(2), bounds.at(3)});
                  else
                    put(current.hitboxes,
                        release_hitbox_record{label_hash, bounds.at(0), bounds.at(1), bounds.at(2), bounds.at(3)});
                  ++hitboxes_total;
                  ++hitbox_count;
                }
              }

            double pivot_x{(static_cast<double>(item->frame_width) - 1.0) / 2.0};
            double pivot_y{(static_cast<double>(item->frame_height) - 1.0) / 2.0};
            if (index < animation.pivots.size())
            {
              pivot_x = animation.pivots.at(index).at(0);
              pivot_y = animation.pivots.at(index).at(1);
            }
            put(current.frames, frame_record{left, top, right, bottom, animation.times.at(index), pivot_x, pivot_y,
                                             hitbox_count > 0 ? hitbox_index : 0, hitbox_count});
            ++frames_total;
            ++index;
          }
          current.clips.try_emplace(item->file).first->second.emplace_back(frame_index, frames_total - frame_index);
        }
        if (item->space == "font")
        {
          current.glyph_spans.insert_or_assign(item->file, std::pair{glyphs_total, item->glyphs.size()});
          const auto canvas_width{static_cast<double>(item->frame_width)};
          const auto canvas_height{static_cast<double>(item->frame_height)};
          for (const auto &entry : item->glyphs)
          {
            put(current.glyphs, glyph_record{entry.character, static_cast<double>(entry.x) / canvas_width,
                                             1.0 - (static_cast<double>(entry.y) / canvas_height),
                                             static_cast<double>(entry.x + entry.width) / canvas_width,
                                             1.0 - (static_cast<double>(entry.y + entry.height) / canvas_height),
                                             static_cast<double>(entry.width), static_cast<double>(entry.height)});
            ++glyphs_total;
          }
        }
      }
      result.push_back(std::move(current));
    }
    return result;
  }

  /**
   * Generates the body of the accessor header: extern declarations for every resource inside the given namespace, plus
   * the per-texture animation and hitbox structs.
   */
  inline std::string accessor_header(const std::vector<const resource *> &resources, const std::string &space)
  {
    std::string result{std::format("namespace {}\n{{\n", space)};
    const auto declare{[&](const std::string &kind, const std::string &type)
                       {
                         std::string block{};
                         for (const auto *item : resources)
                           if (item->space == kind)
                             block += std::format("    extern const cse::{} {};\n", type, item->name);
                         if (!block.empty()) result += std::format("  namespace {}\n  {{\n{}  }}\n", kind, block);
                       }};
    declare("image", "image");
    declare("font", "font");
    {
      std::string structs{};
      std::string externs{};
      for (const auto *item : resources)
      {
        if (item->space != "image" && item->space != "font") continue;
        structs += std::format("      struct {}_animation\n      {{\n", item->name);
        for (const auto &animation : item->animations)
          structs += std::format("        const cse::animation {};\n", animation.name);
        structs += "      };\n";
        externs += std::format("    extern const detail::{}_animation {};\n", item->name, item->name);
      }
      if (!externs.empty())
        result += "  namespace animation\n  {\n    namespace detail\n    {\n" + structs + "    }\n" + externs + "  }\n";
    }
    {
      std::string structs{};
      std::string externs{};
      for (const auto *item : resources)
      {
        if (item->space != "image") continue;
        const auto names{detail::hitbox_names(*item)};
        if (names.empty()) continue;
        structs += std::format("      struct {}_hitbox\n      {{\n", item->name);
        for (const auto &identifier : names) structs += std::format("        const cse::hitbox {};\n", identifier);
        structs += "      };\n";
        externs += std::format("    extern const detail::{}_hitbox {};\n", item->name, item->name);
      }
      if (!externs.empty())
        result += "  namespace hitbox\n  {\n    namespace detail\n    {\n" + structs + "    }\n" + externs + "  }\n";
    }
    declare("sound", "sound");
    declare("music", "music");
    result += "}\n";
    return result;
  }

  /**
   * Generates the body of the accessor source: the pack loaders followed by the definition of every resource, bound to
   * its pack file regions. Layouts come from `layouts` and bindings describe where each blob landed in its written csp
   * container. Sound and music definitions also carry their playing time so the engine can time playback without the
   * audio device.
   */
  inline std::string accessor_source(const std::vector<const resource *> &resources, const std::string &space,
                                     const std::vector<layout> &layouts, const std::vector<binding> &bindings,
                                     const bool debug)
  {
    const auto layout_of{[&](const std::string &pack) -> const layout &
                         {
                           for (const auto &entry : layouts)
                             if (entry.pack == pack) return entry;
                           throw std::runtime_error("Missing layout for pack: " + pack + ".");
                         }};
    const auto binding_of{[&](const std::string &pack) -> const binding &
                          {
                            for (const auto &entry : bindings)
                              if (entry.pack == pack) return entry;
                            throw std::runtime_error("Missing binding for pack: " + pack + ".");
                          }};
    const auto place_of{[&](const resource &item) -> const placement &
                        { return binding_of(item.pack).placements.at(item.file); }};

    std::string loaders{};
    for (const auto &entry : bindings)
    {
      std::string identifier{entry.pack};
      std::ranges::replace(identifier, '.', '_');
      std::ranges::replace(identifier, '-', '_');
      if (debug)
        loaders +=
          std::format("  const loader {}{{\"{}.csp\", {}ull, {}, {}, {}, {}, {}, {}, {}}};\n", identifier, entry.pack,
                      entry.signature, entry.hitboxes.offset, entry.hitboxes.size, entry.frames.offset,
                      entry.frames.size, entry.glyphs.offset, entry.glyphs.size, entry.strings);
      else
        loaders += std::format("  const loader {}{{\"{}.csp\", {}ull, {}, {}, {}, {}, {}, {}}};\n", identifier,
                               entry.pack, entry.signature, entry.hitboxes.offset, entry.hitboxes.size,
                               entry.frames.offset, entry.frames.size, entry.glyphs.offset, entry.glyphs.size);
    }

    std::string result{"namespace cse::resource\n{\n" + loaders + "}\n\n"};
    result += std::format("namespace {}\n{{\n", space);
    {
      std::string block{};
      for (const auto *item : resources)
      {
        if (item->space != "image") continue;
        const auto &place{place_of(*item)};
        block += std::format("    const cse::image {}{{cse::resource::region(\"{}\", {}, {}), {}, {}, {}, {}, {}}};\n",
                             item->name, item->pack + ".csp", place.offset, place.size, item->width, item->height,
                             item->frame_width, item->frame_height, item->channels);
      }
      if (!block.empty()) result += "  namespace image\n  {\n" + block + "  }\n";
    }
    {
      std::string block{};
      for (const auto *item : resources)
      {
        if (item->space != "font") continue;
        const auto &place{place_of(*item)};
        const auto &span{layout_of(item->pack).glyph_spans.at(item->file)};
        block += std::format("    const cse::font {}{{{{cse::resource::region(\"{}\", {}, {}), {}, {}, {}, {}, "
                             "{}}}, cse::resource::glyphs(\"{}\", {}, {})}};\n",
                             item->name, item->pack + ".csp", place.offset, place.size, item->width, item->height,
                             item->frame_width, item->frame_height, item->channels, item->pack + ".csp", span.first,
                             span.second);
      }
      if (!block.empty()) result += "  namespace font\n  {\n" + block + "  }\n";
    }
    {
      std::string block{};
      for (const auto *item : resources)
      {
        if (item->space != "image" && item->space != "font") continue;
        block += std::format("    const detail::{}_animation {}{{", item->name, item->name);
        const auto &list{layout_of(item->pack).clips.at(item->file)};
        for (std::size_t index{}; index < list.size(); ++index)
        {
          const auto &[frame_index, frame_count]{list.at(index)};
          block +=
            std::format("{{cse::resource::frames(\"{}\", {}, {})}}", item->pack + ".csp", frame_index, frame_count);
          if (index + 1 < list.size()) block += ", ";
        }
        block += "};\n";
      }
      if (!block.empty()) result += "  namespace animation\n  {\n" + block + "  }\n";
    }
    {
      std::string block{};
      for (const auto *item : resources)
      {
        if (item->space != "image") continue;
        const auto names{detail::hitbox_names(*item)};
        if (names.empty()) continue;
        block += std::format("    const detail::{}_hitbox {}\n    {{\n", item->name, item->name);
        for (std::size_t index{}; index < names.size(); ++index)
        {
          block += std::format("      {{\"{}\"}}", item->name + "." + names.at(index));
          block += index + 1 < names.size() ? ",\n" : "\n";
        }
        block += "    };\n";
      }
      if (!block.empty()) result += "  namespace hitbox\n  {\n" + block + "  }\n";
    }
    for (const std::string_view resource_space : {"sound", "music"})
    {
      std::string block{};
      for (const auto *item : resources)
        if (item->space == resource_space)
        {
          const auto &place{place_of(*item)};
          block += std::format("    const cse::{} {}{{cse::resource::region(\"{}\", {}, {}), {}}};\n", resource_space,
                               item->name, item->pack + ".csp", place.offset, place.size, item->duration);
        }
      if (!block.empty()) result += std::format("  namespace {}\n  {{\n{}  }}\n", resource_space, block);
    }
    result += "}\n";
    return result;
  }
}
