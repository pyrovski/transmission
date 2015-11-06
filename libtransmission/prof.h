#ifndef TIME_H
#define TIME_H
#include <time.h>

static inline float tsDiff(const struct timespec * tsStart, const struct timespec * tsStop){
  assert(tsStart);
  assert(tsStop);
  float result;
  result = tsStop->tv_sec - tsStart->tv_sec + (tsStop->tv_nsec - tsStart->tv_nsec)/1e9;
  if(tsStop->tv_nsec < tsStart->tv_nsec)
    result += 1.0f;
  return result;
}
#endif
