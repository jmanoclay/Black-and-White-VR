#pragma once

namespace bw::log {

void init();  // opens <game>\bwvr\logs\bwvr.log (truncated each run)
void shutdown();
void write(const char* fmt, ...);

}  // namespace bw::log

#define BWLOG(...) ::bw::log::write(__VA_ARGS__)
