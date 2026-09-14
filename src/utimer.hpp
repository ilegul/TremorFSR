/*
  utimer.hpp
  A scoped RAII timer for wall-clock measurements. It records the time at
  construction and, when it goes out of scope, reports the elapsed
  microseconds: it prints them and, when a long* is passed, also stores
  them there so the caller can use the value. START/STOP perform the same
  measurement without an object, for a region that is not a whole scope.
*/
#ifndef UTIMER_HPP
#define UTIMER_HPP

#include <chrono>
#include <iostream>
#include <string>

#define START(t) auto t = std::chrono::system_clock::now();
#define STOP(t, elapsed)                                                  \
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(   \
                     std::chrono::system_clock::now() - t)                 \
                     .count();

class utimer {
  std::chrono::system_clock::time_point start;
  std::chrono::system_clock::time_point stop;
  std::string message;
  long *us_elapsed;

public:
  explicit utimer(const std::string &m) : message(m), us_elapsed(nullptr) {
    start = std::chrono::system_clock::now();
  }

  utimer(const std::string &m, long *us) : message(m), us_elapsed(us) {
    start = std::chrono::system_clock::now();
  }

  ~utimer() {
    stop = std::chrono::system_clock::now();
    auto musec = std::chrono::duration_cast<std::chrono::microseconds>(
                     stop - start)
                     .count();
    std::cout << message << " computed in " << musec << " usec" << std::endl;
    if (us_elapsed != nullptr)
      *us_elapsed = musec;
  }
};

#endif
