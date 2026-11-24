#pragma once

namespace bw::exe {

// True when the running runblack.exe has the v1.20 layout the bw1-decomp symbols describe.
// Checked by reading RTTI type-descriptor names at their known addresses.
bool matches_v120();

}  // namespace bw::exe
