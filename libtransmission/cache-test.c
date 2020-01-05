#include <event2/buffer.h>
#include <string.h>

#include "transmission.h"
#include "cache.h"
#include "error.h"
#include "session.h"
#include "torrent.h"

#include "libtransmission-test.h"

static tr_session* session;

struct s_args {
  tr_torrent *torrent;
  uint32_t length;
  struct evbuffer *evbuf;
  uint8_t *buf;
  uint8_t *buf2;
  int add_result;
  int write_result;
  int read_result;
  int cmp_result;
  bool done;
} args;

void func(void* arg) {
  args.buf = tr_malloc(args.length);
  args.buf[0] = 1;
  args.buf2 = tr_malloc(args.length);
  // write a block of garbage into the cache
  args.evbuf = evbuffer_new();
  args.add_result = evbuffer_add(args.evbuf, args.buf, args.length);
  args.write_result = tr_cacheWriteBlock(session->cache, args.torrent, 0, 0, args.length, args.evbuf);
  // read the block from the cache
  args.read_result = tr_cacheReadBlock(session->cache, args.torrent, 0, 0, args.length, args.buf2);
  args.cmp_result = memcmp(args.buf, args.buf2, args.length);
  args.done = true;
}

static int test_flush_done(void) {
  args.torrent = libttest_zero_torrent_init(session);
  libttest_zero_torrent_populate(args.torrent, true);
  args.length = args.torrent->blockSize;
  args.done = false;

  tr_runInEventThread(session, func, 0);

  do
    {
      tr_wait_msec(50);
    }
  while (!args.done);
  check_int(args.add_result, ==, 0);
  check_int(args.write_result, ==, 0);
  check_int(args.read_result, ==, 0);
  check_int(args.cmp_result, ==, 0);

  // TODO: test tr_cacheFlushDone()
  
  evbuffer_free(args.evbuf);
  tr_free(args.buf);
  tr_free(args.buf2);
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
