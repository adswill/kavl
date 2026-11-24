// Copyright (c) 2026 Kristijonas Adomaitis (adswill). All rights reserved.
// Use is governed by the KAVL Source Licence (LICENSE).

// ATSC 3.0 A/322 tables used by KAVL (kavl_tables.cpp).
#pragma once

namespace kavl::tables {
// Annex A address table of the 64800-bit code of rate rate15 / 15 (2..13): rows of addresses, one row per group of 360 information bits (Type A
// codes, rates 2..5 and 7: also one row per group of 360 of the first M1 parity bits).
const int* ldpcRows64800(int rate15, const int** lens, int* numRows);
// Non-uniform constellation position vector for 4, 6, 8 (2-D, first quadrant re/im pairs) or 10 (1-D) bits per cell, rate rate15 / 15 (2..13).
const float* nucVectors(int bitsPerCell, int rate15);
}  // namespace kavl::tables
