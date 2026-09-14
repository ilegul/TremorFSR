/*
  sample_io.hpp
  Parsing of the sample line the board sends. The local live path, the
  edge forwarder and the server all read the same
  "sample_id,time_ms,fsr1,fsr2,fsr3" line, so the parsing lives in one
  place and cannot drift between them.
*/
#ifndef SAMPLE_IO_HPP
#define SAMPLE_IO_HPP

#include "analysis.hpp"   /* Sample */

#include <cstdio>
#include <string>

/*
  Fills s from one CSV sample line and returns the raw millisecond
  timestamp in t_ms. Returns false when the line is not a complete
  sample record, so the caller can skip a header or a partial line.
*/
inline bool parse_sample(const std::string& line, Sample& s,
                         unsigned long& t_ms) {
  unsigned long id = 0;
  double a = 0, b = 0, c = 0;

  if (sscanf(line.c_str(), "%lu,%lu,%lf,%lf,%lf",
             &id, &t_ms, &a, &b, &c) != 5)
    return false;

  s.id     = (uint32_t)id;
  s.time_s = t_ms / 1000.0;
  s.fsr[0] = (float)a;
  s.fsr[1] = (float)b;
  s.fsr[2] = (float)c;
  return true;
}

#endif /* SAMPLE_IO_HPP */
