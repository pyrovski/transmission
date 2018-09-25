#include <event2/buffer.h>
#include <string.h>

#include "transmission.h"
#include "cache.h"
#include "error.h"
#include "session.h"
#include "torrent.h"

#include "libtransmission-test.h"

static tr_session* session;

static int test_flush_done(void) {
  tr_torrent *tor = libttest_zero_torrent_init(session);
  libttest_zero_torrent_populate(tor, true);
  const size_t size = tor->blockSize;
  uint8_t *buf = tr_malloc(size);
  buf[0] = 1;
  uint8_t *buf2 = tr_malloc(size);
  // write a block of garbage into the cache
  struct evbuffer *evbuf = evbuffer_new();
  check_int(evbuffer_add(evbuf, buf, size), ==, 0);
  check_int(tr_cacheWriteBlock(session->cache, tor, 0, 0, size, evbuf), ==, 0);
  // read the block from the cache
  check_int(tr_cacheReadBlock(session->cache, tor, 0, 0, size, buf2), ==, 0);
  check_int(memcmp(buf, buf2, size), ==, 0);

  // TODO: test tr_cacheFlushDone()
  
  evbuffer_free(evbuf);
  tr_free(buf);
  tr_free(buf2);
  return 0;
}

int main(void)
{
    testFunc const tests[] =
    {
        test_flush_done
    };

    /* init the session */
    session = libttest_session_init(NULL);

    int ret = runTests(tests, NUM_TESTS(tests));

    libttest_session_close(session);

    return ret;
}
