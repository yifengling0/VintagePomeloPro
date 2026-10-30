/* SPDX-License-Identifier: MIT
 * BC decoder adapted from GameNative/mesa, commit
 * 6bcfbaa701a8d1efa7f6c54145495eda5329f767, wrapper_bcn_decode.comp.
 * Tables and decoding follow bcdec, Copyright (c) 2022 Sergii Kudlai.
 * Full MIT notice is in LICENSE.bcdec beside this file.
 */
const uint bc6h_fields[246] = uint[](
   2070u, 2074u, 2075u, 160u, 164u, 168u, 81u, 2071u,
   70u, 85u, 27u, 71u, 89u, 539u, 74u, 82u,
   1051u, 83u, 1563u, 92u, 2582u, 2071u, 2583u, 112u,
   27u, 539u, 2074u, 116u, 2586u, 1051u, 2070u, 120u,
   1563u, 2587u, 2075u, 97u, 70u, 101u, 71u, 105u,
   74u, 98u, 99u, 92u, 160u, 164u, 168u, 81u,
   5136u, 70u, 69u, 5140u, 27u, 71u, 73u, 5144u,
   539u, 74u, 82u, 1051u, 83u, 1563u, 92u, 160u,
   164u, 168u, 161u, 165u, 169u, 160u, 164u, 168u,
   65u, 5136u, 2071u, 70u, 85u, 5140u, 71u, 73u,
   5144u, 539u, 74u, 66u, 27u, 1051u, 67u, 2070u,
   1563u, 92u, 160u, 164u, 168u, 145u, 5136u, 149u,
   5140u, 153u, 5144u, 160u, 164u, 168u, 65u, 5136u,
   2074u, 70u, 69u, 5140u, 27u, 71u, 89u, 5144u,
   74u, 66u, 539u, 1051u, 67u, 2075u, 1563u, 92u,
   160u, 164u, 168u, 129u, 21536u, 133u, 21540u, 137u,
   21544u, 144u, 2074u, 148u, 2070u, 152u, 2075u, 81u,
   2071u, 70u, 85u, 27u, 71u, 89u, 539u, 74u,
   82u, 1051u, 83u, 1563u, 92u, 160u, 164u, 168u,
   65u, 21600u, 69u, 21604u, 73u, 21608u, 128u, 2071u,
   2074u, 132u, 1051u, 2070u, 136u, 1563u, 2075u, 97u,
   70u, 85u, 27u, 71u, 89u, 539u, 74u, 98u,
   99u, 92u, 128u, 27u, 2074u, 132u, 2582u, 2070u,
   136u, 2583u, 2075u, 81u, 2071u, 70u, 101u, 71u,
   89u, 539u, 74u, 82u, 1051u, 83u, 1563u, 92u,
   128u, 539u, 2074u, 132u, 2586u, 2070u, 136u, 2587u,
   2075u, 81u, 2071u, 70u, 85u, 27u, 71u, 105u,
   74u, 82u, 1051u, 83u, 1563u, 92u, 96u, 2071u,
   27u, 539u, 2074u, 100u, 2582u, 2586u, 1051u, 2070u,
   104u, 2583u, 1563u, 2587u, 2075u, 97u, 70u, 101u,
   71u, 105u, 74u, 98u, 99u, 92u
);
const uint bc6h_modes[32] = uint[](
   0x00001400u, 0x00011814u, 0x0002132cu, 0x000a063fu, 0x00001400u, 0x00011814u, 0x00031545u, 0x000b095au,
   0x00001400u, 0x00011814u, 0x00041563u, 0x000c0978u, 0x00001400u, 0x00011814u, 0x00051481u, 0x000d0995u,
   0x00001400u, 0x00011814u, 0x0006149eu, 0xffffffffu, 0x00001400u, 0x00011814u, 0x000716b2u, 0xffffffffu,
   0x00001400u, 0x00011814u, 0x000816c8u, 0xffffffffu, 0x00001400u, 0x00011814u, 0x000918deu, 0xffffffffu
);
const uint bc6h_part[32] = uint[](
   0x50505050u, 0x40404040u, 0x54545454u, 0x54505040u, 0x50404000u, 0x55545450u, 0x55545040u, 0x54504000u,
   0x50400000u, 0x55555450u, 0x55544000u, 0x54400000u, 0x55555440u, 0x55550000u, 0x55555500u, 0x55000000u,
   0x55150100u, 0x00004054u, 0x15010000u, 0x00405054u, 0x00004050u, 0x15050100u, 0x05010000u, 0x40505054u,
   0x00404050u, 0x05010100u, 0x14141414u, 0x05141450u, 0x01155440u, 0x00555500u, 0x15014054u, 0x05414150u
);
const uint bc6h_fix[32] = uint[](
   0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u,
   0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u,
   0x8001u, 0x0005u, 0x0101u, 0x0005u, 0x0005u, 0x0101u, 0x0101u, 0x8001u,
   0x0005u, 0x0101u, 0x0005u, 0x0005u, 0x0101u, 0x0101u, 0x0005u, 0x0005u
);
const uint bc7_part[128] = uint[](
   0x50505050u, 0x40404040u, 0x54545454u, 0x54505040u, 0x50404000u, 0x55545450u, 0x55545040u, 0x54504000u,
   0x50400000u, 0x55555450u, 0x55544000u, 0x54400000u, 0x55555440u, 0x55550000u, 0x55555500u, 0x55000000u,
   0x55150100u, 0x00004054u, 0x15010000u, 0x00405054u, 0x00004050u, 0x15050100u, 0x05010000u, 0x40505054u,
   0x00404050u, 0x05010100u, 0x14141414u, 0x05141450u, 0x01155440u, 0x00555500u, 0x15014054u, 0x05414150u,
   0x44444444u, 0x55005500u, 0x11441144u, 0x05055050u, 0x05500550u, 0x11114444u, 0x41144114u, 0x44111144u,
   0x15055054u, 0x01055040u, 0x05041050u, 0x05455150u, 0x14414114u, 0x50050550u, 0x41411414u, 0x00141400u,
   0x00041504u, 0x00105410u, 0x10541000u, 0x04150400u, 0x50410514u, 0x41051450u, 0x05415014u, 0x14054150u,
   0x41050514u, 0x41505014u, 0x40011554u, 0x54150140u, 0x50505500u, 0x00555050u, 0x15151010u, 0x54540404u,
   0xaa685050u, 0x6a5a5040u, 0x5a5a4200u, 0x5450a0a8u, 0xa5a50000u, 0xa0a05050u, 0x5555a0a0u, 0x5a5a5050u,
   0xaa550000u, 0xaa555500u, 0xaaaa5500u, 0x90909090u, 0x94949494u, 0xa4a4a4a4u, 0xa9a59450u, 0x2a0a4250u,
   0xa5945040u, 0x0a425054u, 0xa5a5a500u, 0x55a0a0a0u, 0xa8a85454u, 0x6a6a4040u, 0xa4a45000u, 0x1a1a0500u,
   0x0050a4a4u, 0xaaa59090u, 0x14696914u, 0x69691400u, 0xa08585a0u, 0xaa821414u, 0x50a4a450u, 0x6a5a0200u,
   0xa9a58000u, 0x5090a0a8u, 0xa8a09050u, 0x24242424u, 0x00aa5500u, 0x24924924u, 0x24499224u, 0x50a50a50u,
   0x500aa550u, 0xaaaa4444u, 0x66660000u, 0xa5a0a5a0u, 0x50a050a0u, 0x69286928u, 0x44aaaa44u, 0x66666600u,
   0xaa444444u, 0x54a854a8u, 0x95809580u, 0x96969600u, 0xa85454a8u, 0x80959580u, 0xaa141414u, 0x96960000u,
   0xaaaa1414u, 0xa05050a0u, 0xa0a5a5a0u, 0x96000000u, 0x40804080u, 0xa9a8a9a8u, 0xaaaaaa44u, 0x2a4a5254u
);
const uint bc7_fix[128] = uint[](
   0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u,
   0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u,
   0x8001u, 0x0005u, 0x0101u, 0x0005u, 0x0005u, 0x0101u, 0x0101u, 0x8001u,
   0x0005u, 0x0101u, 0x0005u, 0x0005u, 0x0101u, 0x0101u, 0x0005u, 0x0005u,
   0x8001u, 0x8001u, 0x0041u, 0x0101u, 0x0005u, 0x0101u, 0x8001u, 0x8001u,
   0x0005u, 0x0101u, 0x0005u, 0x0005u, 0x0005u, 0x8001u, 0x8001u, 0x0041u,
   0x0041u, 0x0005u, 0x0041u, 0x0101u, 0x8001u, 0x8001u, 0x0005u, 0x0005u,
   0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x8001u, 0x0005u, 0x0005u, 0x8001u,
   0x8009u, 0x0109u, 0x8101u, 0x8009u, 0x8101u, 0x8009u, 0x8009u, 0x8101u,
   0x8101u, 0x8101u, 0x8041u, 0x8041u, 0x8041u, 0x8021u, 0x8009u, 0x0109u,
   0x8009u, 0x0109u, 0x8101u, 0x8009u, 0x8009u, 0x0109u, 0x8041u, 0x0501u,
   0x0029u, 0x8101u, 0x0141u, 0x0441u, 0x8101u, 0x8021u, 0x8401u, 0x8101u,
   0x8101u, 0x8009u, 0x8009u, 0x0421u, 0x0441u, 0x0501u, 0x0301u, 0x8401u,
   0x8041u, 0x8009u, 0x8101u, 0x8021u, 0x8009u, 0x8041u, 0x8041u, 0x8101u,
   0x8009u, 0x8009u, 0x8021u, 0x8021u, 0x8021u, 0x8101u, 0x8021u, 0x8401u,
   0x8021u, 0x8401u, 0x8101u, 0xa001u, 0x8009u, 0x9001u, 0x8009u, 0x0109u
);

uvec4 blk;
uint bitpos;

uint rb(uint n)
{
   if (n == 0u)
      return 0u;
   uint word = bitpos >> 5, off = bitpos & 31u;
   uint v = blk[word] >> off;
   if (off + n > 32u && word < 3u)
      v |= blk[word + 1u] << (32u - off);
   bitpos += n;
   return n == 32u ? v : (v & ((1u << n) - 1u));
}

uint rbr(uint n)
{
   uint v = rb(n), r = 0u;
   for (uint i = 0u; i < n; i++)
      r |= ((v >> i) & 1u) << (n - 1u - i);
   return r;
}

uint texels[16];
uint texels_hi[16];

uint rgb565_r(uint c) { return ((c >> 11) & 31u); }
uint rgb565_g(uint c) { return ((c >> 5) & 63u); }
uint rgb565_b(uint c) { return (c & 31u); }

void decode_color(uint w0, uint w1, bool only_opaque, bool explicit_alpha)
{
   uint c0 = w0 & 0xffffu, c1 = w0 >> 16;
   uint r0 = rgb565_r(c0), g0 = rgb565_g(c0), b0 = rgb565_b(c0);
   uint r1 = rgb565_r(c1), g1 = rgb565_g(c1), b1 = rgb565_b(c1);
   uint ref[4];
   ref[0] = 0xff000000u | (((b0 * 527u + 23u) >> 6) << 16) | (((g0 * 259u + 33u) >> 6) << 8) | ((r0 * 527u + 23u) >> 6);
   ref[1] = 0xff000000u | (((b1 * 527u + 23u) >> 6) << 16) | (((g1 * 259u + 33u) >> 6) << 8) | ((r1 * 527u + 23u) >> 6);
   if (c0 > c1 || only_opaque) {
      ref[2] = 0xff000000u | ((((2u * b0 + b1) * 351u + 61u) >> 7) << 16) |
               ((((2u * g0 + g1) * 2763u + 1039u) >> 11) << 8) | (((2u * r0 + r1) * 351u + 61u) >> 7);
      ref[3] = 0xff000000u | ((((b0 + b1 * 2u) * 351u + 61u) >> 7) << 16) |
               ((((g0 + g1 * 2u) * 2763u + 1039u) >> 11) << 8) | (((r0 + r1 * 2u) * 351u + 61u) >> 7);
   } else {
      ref[2] = 0xff000000u | ((((b0 + b1) * 1053u + 125u) >> 8) << 16) |
               ((((g0 + g1) * 4145u + 1019u) >> 11) << 8) | (((r0 + r1) * 1053u + 125u) >> 8);
      ref[3] = 0u;
   }
   for (uint i = 0u; i < 16u; i++) {
      uint c = ref[(w1 >> (2u * i)) & 3u];
      if (explicit_alpha) {
         uint a = ((i < 8u ? blk.x : blk.y) >> (4u * (i & 7u))) & 15u;
         c = (c & 0x00ffffffu) | ((a * 17u) << 24);
      }
      texels[i] = c;
   }
}

/* BC3/BC4 style 8-value channel block, bcdec precise integer weights. */
void decode_channel(uint w0, uint w1, bool is_signed, out int vals[16])
{
   int a[8];
   if (is_signed) {
      a[0] = max(int(w0 << 24) >> 24, -127);
      a[1] = max(int(w0 << 16) >> 24, -127);
   } else {
      a[0] = int(w0 & 0xffu);
      a[1] = int((w0 >> 8) & 0xffu);
   }
   if (a[0] > a[1]) {
      a[2] = (56173 * a[0] + 9363 * a[1] + 32768) >> 16;
      a[3] = (46812 * a[0] + 18724 * a[1] + 32768) >> 16;
      a[4] = (37450 * a[0] + 28086 * a[1] + 32768) >> 16;
      a[5] = (28086 * a[0] + 37450 * a[1] + 32768) >> 16;
      a[6] = (18724 * a[0] + 46812 * a[1] + 32768) >> 16;
      a[7] = (9363 * a[0] + 56173 * a[1] + 32768) >> 16;
   } else {
      a[2] = (52429 * a[0] + 13107 * a[1] + 32768) >> 16;
      a[3] = (39321 * a[0] + 26215 * a[1] + 32768) >> 16;
      a[4] = (26215 * a[0] + 39321 * a[1] + 32768) >> 16;
      a[5] = (13107 * a[0] + 52429 * a[1] + 32768) >> 16;
      a[6] = is_signed ? -127 : 0;
      a[7] = is_signed ? 127 : 255;
   }
   uint lo = w0 >> 16, hi = w1;
   for (uint i = 0u; i < 16u; i++) {
      uint bit = 3u * i, idx;
      if (bit + 3u <= 16u)
         idx = (lo >> bit) & 7u;
      else if (bit >= 16u)
         idx = (hi >> (bit - 16u)) & 7u;
      else
         idx = ((lo >> bit) | (hi << (16u - bit))) & 7u;
      vals[i] = a[idx];
   }
}

/* bcdec's BC3 alpha uses the plain /7 and /5 interpolation. */
void decode_alpha_bc3(uint w0, uint w1)
{
   uint a[8];
   a[0] = w0 & 0xffu;
   a[1] = (w0 >> 8) & 0xffu;
   if (a[0] > a[1]) {
      a[2] = (6u * a[0] + a[1]) / 7u;
      a[3] = (5u * a[0] + 2u * a[1]) / 7u;
      a[4] = (4u * a[0] + 3u * a[1]) / 7u;
      a[5] = (3u * a[0] + 4u * a[1]) / 7u;
      a[6] = (2u * a[0] + 5u * a[1]) / 7u;
      a[7] = (a[0] + 6u * a[1]) / 7u;
   } else {
      a[2] = (4u * a[0] + a[1]) / 5u;
      a[3] = (3u * a[0] + 2u * a[1]) / 5u;
      a[4] = (2u * a[0] + 3u * a[1]) / 5u;
      a[5] = (a[0] + 4u * a[1]) / 5u;
      a[6] = 0u;
      a[7] = 255u;
   }
   uint lo = w0 >> 16, hi = w1;
   for (uint i = 0u; i < 16u; i++) {
      uint bit = 3u * i, idx;
      if (bit + 3u <= 16u)
         idx = (lo >> bit) & 7u;
      else if (bit >= 16u)
         idx = (hi >> (bit - 16u)) & 7u;
      else
         idx = ((lo >> bit) | (hi << (16u - bit))) & 7u;
      texels[i] = (texels[i] & 0x00ffffffu) | (a[idx] << 24);
   }
}

const uint w3[8] = uint[](0u, 9u, 18u, 27u, 37u, 46u, 55u, 64u);
const uint w4[16] = uint[](0u, 4u, 9u, 13u, 17u, 21u, 26u, 30u, 34u, 38u, 43u, 47u, 51u, 55u, 60u, 64u);
const uint w2[4] = uint[](0u, 21u, 43u, 64u);

int extend_sign(int v, int bits) { return (v << (32 - bits)) >> (32 - bits); }

int unquantize6(int v, int bits, bool s)
{
   if (!s) {
      if (bits >= 15) return v;
      if (v == 0) return 0;
      if (v == (1 << bits) - 1) return 0xffff;
      return ((v << 16) + 0x8000) >> bits;
   }
   if (bits >= 16) return v;
   bool neg = v < 0;
   if (neg) v = -v;
   int u;
   if (v == 0) u = 0;
   else if (v >= (1 << (bits - 1)) - 1) u = 0x7fff;
   else u = ((v << 15) + 0x4000) >> (bits - 1);
   return neg ? -u : u;
}

uint finish6(int v, bool s)
{
   if (!s)
      return uint((v * 31) >> 6) & 0xffffu;
   v = v < 0 ? -(((-v) * 31) >> 5) : (v * 31) >> 5;
   uint sign = 0u;
   if (v < 0) { sign = 0x8000u; v = -v; }
   return (sign | uint(v)) & 0xffffu;
}

const int bc6h_bits[56] = int[](
   10, 7, 11, 11, 11, 9, 8, 8, 8, 6, 10, 11, 12, 16,
   5, 6, 5, 4, 4, 5, 6, 5, 5, 6, 10, 9, 8, 4,
   5, 6, 4, 5, 4, 5, 5, 6, 5, 6, 10, 9, 8, 4,
   5, 6, 4, 4, 5, 5, 5, 5, 6, 6, 10, 9, 8, 4);

void decode_bc6h(bool s)
{
   bitpos = 0u;
   uint raw = rb(2u);
   if (raw > 1u)
      raw |= rb(3u) << 2;
   uint key = bc6h_modes[raw];
   if (key == 0xffffffffu) {
      for (uint i = 0u; i < 16u; i++) { texels[i] = 0u; texels_hi[i] = 0x3c000000u; }
      return;
   }
   int e[12];
   for (uint i = 0u; i < 12u; i++) e[i] = 0;
   uint part_id = 0u;
   uint start = key & 0xffu, count = (key >> 8) & 0xffu;
   int mode = int(key >> 16);
   for (uint f = 0u; f < count; f++) {
      uint d = bc6h_fields[start + f];
      uint t = d & 15u, n = (d >> 4) & 31u, sh = (d >> 9) & 31u;
      uint v = ((d >> 14) & 1u) != 0u ? rbr(n) : rb(n);
      if (t == 12u)
         part_id = v;
      else
         e[t] |= int(v << sh);
   }
   int parts = mode >= 10 ? 0 : 1;
   int b0 = bc6h_bits[mode];
   if (s) {
      e[0] = extend_sign(e[0], b0);
      e[4] = extend_sign(e[4], b0);
      e[8] = extend_sign(e[8], b0);
   }
   int ne = (parts + 1) * 2;
   if ((mode != 9 && mode != 10) || s) {
      for (int i = 1; i < ne; i++) {
         e[i] = extend_sign(e[i], bc6h_bits[14 + mode]);
         e[4 + i] = extend_sign(e[4 + i], bc6h_bits[28 + mode]);
         e[8 + i] = extend_sign(e[8 + i], bc6h_bits[42 + mode]);
      }
   }
   if (mode != 9 && mode != 10) {
      for (int i = 1; i < ne; i++) {
         for (int c = 0; c < 3; c++) {
            int v = (e[c * 4 + i] + e[c * 4]) & ((1 << b0) - 1);
            e[c * 4 + i] = s ? extend_sign(v, b0) : v;
         }
      }
   }
   for (int i = 0; i < ne; i++)
      for (int c = 0; c < 3; c++)
         e[c * 4 + i] = unquantize6(e[c * 4 + i], b0, s);

   for (uint t = 0u; t < 16u; t++) {
      uint subset, nbits = mode >= 10 ? 4u : 3u;
      if (mode >= 10) {
         subset = 0u;
         if (t == 0u) nbits--;
      } else {
         subset = (bc6h_part[part_id] >> (2u * t)) & 1u;
         if (((bc6h_fix[part_id] >> t) & 1u) != 0u) nbits--;
      }
      uint idx = rb(nbits);
      int wgt = int(mode >= 10 ? w4[idx] : w3[idx]);
      int ep = int(subset) * 2;
      uint ch[3];
      for (int c = 0; c < 3; c++) {
         int v = (e[c * 4 + ep] * (64 - wgt) + e[c * 4 + ep + 1] * wgt + 32) >> 6;
         ch[c] = finish6(v, s);
      }
      texels[t] = ch[0] | (ch[1] << 16);
      texels_hi[t] = ch[2] | 0x3c000000u;
   }
}

const int bc7_bits[16] = int[](4, 6, 5, 7, 5, 7, 7, 5,  0, 0, 0, 0, 6, 8, 7, 5);
const uint bc7_pbits = 0xcbu;

void decode_bc7()
{
   bitpos = 0u;
   int mode = 0;
   while (mode < 8 && rb(1u) == 0u)
      mode++;
   if (mode >= 8) {
      for (uint i = 0u; i < 16u; i++) texels[i] = 0u;
      return;
   }
   uint part_id = 0u, parts = 1u, rotation = 0u, isb = 0u;
   if (mode == 0 || mode == 1 || mode == 2 || mode == 3 || mode == 7) {
      parts = (mode == 0 || mode == 2) ? 3u : 2u;
      part_id = rb(mode == 0 ? 4u : 6u);
   }
   uint ne = parts * 2u;
   if (mode == 4 || mode == 5) {
      rotation = rb(2u);
      if (mode == 4)
         isb = rb(1u);
   }
   int ep[24];
   for (int i = 0; i < 24; i++) ep[i] = 0;
   for (uint c = 0u; c < 3u; c++)
      for (uint j = 0u; j < ne; j++)
         ep[j * 4u + c] = int(rb(uint(bc7_bits[mode])));
   if (bc7_bits[8 + mode] > 0)
      for (uint j = 0u; j < ne; j++)
         ep[j * 4u + 3u] = int(rb(uint(bc7_bits[8 + mode])));
   if (mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7) {
      for (uint i = 0u; i < ne * 4u; i++)
         ep[i] <<= 1;
      if (mode == 1) {
         int p0 = int(rb(1u)), p1 = int(rb(1u));
         for (int k = 0; k < 3; k++) {
            ep[0 + k] |= p0; ep[4 + k] |= p0;
            ep[8 + k] |= p1; ep[12 + k] |= p1;
         }
      } else if (((bc7_pbits >> mode) & 1u) != 0u) {
         for (uint i = 0u; i < ne; i++) {
            int p = int(rb(1u));
            for (uint k = 0u; k < 4u; k++)
               ep[i * 4u + k] |= p;
         }
      }
   }
   int pb = int((bc7_pbits >> mode) & 1u);
   for (uint i = 0u; i < ne; i++) {
      int j = bc7_bits[mode] + pb;
      for (uint k = 0u; k < 3u; k++) {
         int v = ep[i * 4u + k] << (8 - j);
         ep[i * 4u + k] = (v | (v >> j)) & 0xff;
      }
      j = bc7_bits[8 + mode] + pb;
      int v = ep[i * 4u + 3u] << (8 - j);
      ep[i * 4u + 3u] = (v | (v >> j)) & 0xff;
   }
   if (bc7_bits[8 + mode] == 0)
      for (uint j = 0u; j < ne; j++)
         ep[j * 4u + 3u] = 255;

   uint ib = (mode == 0 || mode == 1) ? 3u : (mode == 6 ? 4u : 2u);
   uint ib2 = mode == 4 ? 3u : (mode == 5 ? 2u : 0u);
   uint idx1[16], sub[16];
   uint ptab = parts == 3u ? 64u + part_id : part_id;
   for (uint t = 0u; t < 16u; t++) {
      uint n = ib;
      if (parts == 1u) {
         sub[t] = 0u;
         if (t == 0u) n--;
      } else {
         sub[t] = (bc7_part[ptab] >> (2u * t)) & 3u;
         if (((bc7_fix[ptab] >> t) & 1u) != 0u) n--;
      }
      idx1[t] = rb(n);
   }
   for (uint t = 0u; t < 16u; t++) {
      uint e0 = sub[t] * 8u;
      uint i1 = idx1[t];
      uint wc = ib == 2u ? w2[i1] : (ib == 3u ? w3[i1] : w4[i1]);
      uint wa = wc;
      if (ib2 != 0u) {
         uint i2 = rb(t == 0u ? ib2 - 1u : ib2);
         uint w2v = ib2 == 2u ? w2[i2] : w3[i2];
         if (isb != 0u) { wa = wc; wc = w2v; }
         else wa = w2v;
      }
      uint c[4];
      for (uint k = 0u; k < 3u; k++)
         c[k] = uint((ep[e0 + k] * (64 - int(wc)) + ep[e0 + 4u + k] * int(wc) + 32) >> 6);
      c[3] = uint((ep[e0 + 3u] * (64 - int(wa)) + ep[e0 + 7u] * int(wa) + 32) >> 6);
      if (rotation == 1u) { uint x = c[0]; c[0] = c[3]; c[3] = x; }
      else if (rotation == 2u) { uint x = c[1]; c[1] = c[3]; c[3] = x; }
      else if (rotation == 3u) { uint x = c[2]; c[2] = c[3]; c[3] = x; }
      texels[t] = c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24);
   }
}

uint b10g11r11(uint h, uint mbits)
{
   uint drop = 10u - mbits;
   uint maxv = (30u << mbits) | ((1u << mbits) - 1u);
   if ((h & 0x8000u) != 0u || (h & 0x7fffu) > 0x7c00u)
      return 0u;
   return min(((h & 0x7fffu) + (1u << (drop - 1u))) >> drop, maxv);
}

