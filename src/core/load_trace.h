#pragma once

// The game's file opens (LHOSFile::Open) and its profile reads from the registry (the multiplayer
// library's LHNetGetCurrentProfile*, through the exe's imports) are timed: any over 2 ms is logged
// (the first few), and the totals with the 5-second stats. They were most of the frame spikes as
// play starts outside the 3D pass (player, 2026-10-30).
namespace bw::load_trace {

void install();      // after MH_Initialize
void trace(int n);   // log the next n calls, slow or not (stereo load_trace <n>)
void report();       // the totals since the last report, then zeroed (with the 5-second stats)

}  // namespace bw::load_trace
