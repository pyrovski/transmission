/*
 * This file Copyright (C) 2007-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 * $Id$
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h> /* bsearch () */
#include <string.h> /* memcmp () */
#include <stdio.h>

#include "transmission.h"
#include "cache.h" /* tr_cacheReadBlock () */
#include "crypto-utils.h"
#include "error.h"
#include "fdlimit.h"
#include "file.h"
#include "inout.h"
#include "log.h"
#include "peer-common.h" /* MAX_BLOCK_SIZE */
#include "stats.h" /* tr_statsFileCreated () */
#include "torrent.h"
#include "utils.h"

/****
*****  Low-level IO functions
****/

enum
{
  TR_IO_READ,
  TR_IO_PREFETCH,
  /* Any operations that require write access must follow TR_IO_WRITE. */
  TR_IO_WRITE
};

static int
_readOrWriteBytes (tr_session       * session,
		   tr_torrent       * tor,
		   int                ioMode,
		   tr_file_index_t    fileIndex,
		   uint64_t           fileOffset,
		   void             * buf,
		   struct evbuffer  * evbuf,
		   size_t             buflen)
{
  assert((buf || evbuf) && !(buf && evbuf) || ioMode == TR_IO_PREFETCH);
  if(evbuf && ioMode != TR_IO_WRITE){
    fprintf(stderr, "Attempted readv; not supported yet.\n");
    exit(1);
  }    

  tr_sys_file_t fd;
  int err = 0;
  const bool doWrite = ioMode >= TR_IO_WRITE;
  const tr_info * const info = &tor->info;
  const tr_file * const file = &info->files[fileIndex];

  assert (fileIndex < info->fileCount);
  assert (!file->length || (fileOffset < file->length));
  assert (fileOffset + buflen <= file->length);

  if (!file->length)
    return 0;

  /***
  ****  Find the fd
  ***/

  fd = tr_fdFileGetCached (session, tr_torrentId (tor), fileIndex, doWrite);
  if (fd == TR_BAD_SYS_FILE)
    {
      /* it's not cached, so open/create it now */
      char * subpath;
      const char * base;

      /* see if the file exists... */
      if (!tr_torrentFindFile2 (tor, fileIndex, &base, &subpath, NULL))
        {
          /* we can't read a file that doesn't exist... */
          if (!doWrite)
            err = ENOENT;

          /* figure out where the file should go, so we can create it */
          base = tr_torrentGetCurrentDir (tor);
          subpath = tr_sessionIsIncompleteFileNamingEnabled (tor->session)
                  ? tr_torrentBuildPartial (tor, fileIndex)
                  : tr_strdup (file->name);

        }

      if (!err)
        {
          /* open (and maybe create) the file */
          char * filename = tr_buildPath (base, subpath, NULL);
          const int prealloc = file->dnd || !doWrite
                             ? TR_PREALLOCATE_NONE
                             : tor->session->preallocationMode;
          if (((fd = tr_fdFileCheckout (session, tor->uniqueId, fileIndex,
                                        filename, doWrite,
                                        prealloc, file->length))) == TR_BAD_SYS_FILE)
            {
              err = errno;
              tr_logAddTorErr (tor, "tr_fdFileCheckout failed for \"%s\": %s",
                         filename, tr_strerror (err));
            }
          else if (doWrite)
            {
              /* make a note that we just created a file */
              tr_statsFileCreated (tor->session);
            }

          tr_free (filename);
        }

      tr_free (subpath);
    }

  /***
  ****  Use the fd
  ***/

  if (!err)
    {
      tr_error * error = NULL;

      if (ioMode == TR_IO_READ)
        {
          if (!tr_sys_file_read_at (fd, buf, buflen, fileOffset, NULL, &error))
            {
              err = error->code;
              tr_logAddTorErr (tor, "read failed for \"%s\": %s", file->name, error->message);
              tr_error_free (error);
            }
        }
      else if (ioMode == TR_IO_WRITE)
        {
	  int status = 0;
	  if(buf)
	    status = tr_sys_file_write_at (fd, buf, buflen, fileOffset, NULL, &error);
	  else if(evbuf)
	    status = tr_sys_file_writev_at (fd, evbuf, buflen, fileOffset, NULL, &error);
          if (!status)
            {
              err = error->code;
              tr_logAddTorErr (tor, "write failed for \"%s\": %s", file->name, error->message);
              tr_error_free (error);
            }
        }
      else if (ioMode == TR_IO_PREFETCH)
        {
          tr_sys_file_prefetch (fd, fileOffset, buflen, NULL);
        }
      else
        {
          abort ();
        }
    }

  return err;
}

/* returns 0 on success, or an errno on failure */
static int
readOrWriteBytes (tr_session       * session,
                  tr_torrent       * tor,
                  int                ioMode,
                  tr_file_index_t    fileIndex,
                  uint64_t           fileOffset,
                  void             * buf,
                  size_t             buflen)
{
  return _readOrWriteBytes(session, tor, ioMode, fileIndex, fileOffset, buf, NULL, buflen);
}

/* returns 0 on success, or an errno on failure */
static int
readvOrWritevBytes (tr_session       * session,
		    tr_torrent       * tor,
		    int                ioMode,
		    tr_file_index_t    fileIndex,
		    uint64_t           fileOffset,
		    struct evbuffer  * buf,
		    size_t             buflen)
{
  return _readOrWriteBytes(session, tor, ioMode, fileIndex, fileOffset, NULL, buf, buflen);
}

static int
compareOffsetToFile (const void * a, const void * b)
{
  const uint64_t  offset = * (const uint64_t*)a;
  const tr_file * file = b;

  if (offset < file->offset)
    return -1;

  if (offset >= file->offset + file->length)
    return 1;

  return 0;
}

void
tr_ioFindFileLocation (const tr_torrent * tor,
                       tr_piece_index_t   pieceIndex,
                       uint32_t           pieceOffset,
                       tr_file_index_t  * fileIndex,
                       uint64_t         * fileOffset)
{
  const uint64_t  offset = tr_pieceOffset (tor, pieceIndex, pieceOffset, 0);
  const tr_file * file;

  assert (tr_isTorrent (tor));
  assert (offset < tor->info.totalSize);

  file = bsearch (&offset,
                  tor->info.files, tor->info.fileCount, sizeof (tr_file),
                  compareOffsetToFile);

  assert (file != NULL);

  *fileIndex = file - tor->info.files;
  *fileOffset = offset - file->offset;

  assert (*fileIndex < tor->info.fileCount);
  assert (*fileOffset < file->length);
  assert (tor->info.files[*fileIndex].offset + *fileOffset == offset);
}

/* returns 0 on success, or an errno on failure */
static int
_readOrWritePiece (tr_torrent       * tor,
		   int                ioMode,
		   tr_piece_index_t   pieceIndex,
		   uint32_t           pieceOffset,
		   uint8_t          * buf,
		   struct evbuffer  * evbuf,
		   size_t             buflen)
{
  assert(((buf || evbuf) && !(buf && evbuf)) || ioMode == TR_IO_PREFETCH);

  int err = 0;
  tr_file_index_t fileIndex;
  uint64_t fileOffset;
  const tr_info * info = &tor->info;

  if (pieceIndex >= tor->info.pieceCount)
    return EINVAL;

  tr_ioFindFileLocation (tor, pieceIndex, pieceOffset,
                         &fileIndex, &fileOffset);

  while (buflen && !err)
    {
      const tr_file * file = &info->files[fileIndex];
      const uint64_t bytesThisPass = MIN (buflen, file->length - fileOffset);

      if(evbuf){
	//!@todo make sure buffer is advanced
	err = readvOrWritevBytes (tor->session, tor, ioMode, fileIndex, fileOffset, evbuf, bytesThisPass);
      } else {
	err = readOrWriteBytes (tor->session, tor, ioMode, fileIndex, fileOffset, buf, bytesThisPass);
	buf += bytesThisPass;
      }

      buflen -= bytesThisPass;

      //!@todo this assumes bytes remain due to a file boundary.
      fileIndex++;
      fileOffset = 0;

      if ((err != 0) && (ioMode == TR_IO_WRITE) && (tor->error != TR_STAT_LOCAL_ERROR))
        {
          char * path = tr_buildPath (tor->downloadDir, file->name, NULL);
          tr_torrentSetLocalError (tor, "%s (%s)", tr_strerror (err), path);
          tr_free (path);
        }
    }

  return err;
}

/* returns 0 on success, or an errno on failure */
static int
readOrWritePiece (tr_torrent       * tor,
                  int                ioMode,
                  tr_piece_index_t   pieceIndex,
                  uint32_t           pieceOffset,
                  uint8_t          * buf,
                  size_t             buflen)
{
  return _readOrWritePiece(tor, ioMode, pieceIndex, pieceOffset, buf, NULL, buflen);
}

/* returns 0 on success, or an errno on failure */
static int
readvOrWritevPiece (tr_torrent       * tor,
		    int                ioMode,
		    tr_piece_index_t   pieceIndex,
		    uint32_t           pieceOffset,
		    struct evbuffer  * buf,
		    size_t             buflen)
{
  return _readOrWritePiece(tor, ioMode, pieceIndex, pieceOffset, NULL, buf, buflen);
}

int
tr_ioRead (tr_torrent       * tor,
           tr_piece_index_t   pieceIndex,
           uint32_t           begin,
           uint32_t           len,
           uint8_t          * buf)
{
  return readOrWritePiece (tor, TR_IO_READ, pieceIndex, begin, buf, len);
}

int
tr_ioPrefetch (tr_torrent       * tor,
               tr_piece_index_t   pieceIndex,
               uint32_t           begin,
               uint32_t           len)
{
  return readOrWritePiece (tor, TR_IO_PREFETCH, pieceIndex, begin, NULL, len);
}

int
tr_ioWrite (tr_torrent       * tor,
            tr_piece_index_t   pieceIndex,
            uint32_t           begin,
            uint32_t           len,
            const uint8_t    * buf)
{
  return readOrWritePiece (tor, TR_IO_WRITE, pieceIndex, begin, (uint8_t*)buf, len);
}

int
tr_ioWritev (tr_torrent         * tor,
	     tr_piece_index_t     pieceIndex,
	     uint32_t             begin,
	     uint32_t             len,
	     struct evbuffer    * buf)
{
  return readvOrWritevPiece (tor, TR_IO_WRITE, pieceIndex, begin, buf, len);
}

/****
*****
****/

static bool
recalculateHash (tr_torrent * tor, tr_piece_index_t pieceIndex, uint8_t * setme)
{
  size_t   bytesLeft;
  uint32_t offset = 0;
  bool  success = true;
  const size_t buflen = tor->blockSize;
  void * buffer = tr_valloc (buflen);
  tr_sha1_ctx_t sha;

  assert (tor != NULL);
  assert (pieceIndex < tor->info.pieceCount);
  assert (buffer != NULL);
  assert (buflen > 0);
  assert (setme != NULL);

  sha = tr_sha1_init ();
  bytesLeft = tr_torPieceCountBytes (tor, pieceIndex);

  tr_ioPrefetch (tor, pieceIndex, offset, bytesLeft);

  while (bytesLeft)
    {
      const size_t len = MIN (bytesLeft, buflen);
      success = !tr_cacheReadBlock (tor->session->cache, tor, pieceIndex, offset, len, buffer);
      if (!success)
        break;
      tr_sha1_update (sha, buffer, len);
      offset += len;
      bytesLeft -= len;
    }

  tr_sha1_final (sha, success ? setme : NULL);

  tr_free (buffer);
  return success;
}

bool
tr_ioTestPiece (tr_torrent * tor, tr_piece_index_t piece)
{
  uint8_t hash[SHA_DIGEST_LENGTH];

  return recalculateHash (tor, piece, hash)
      && !memcmp (hash, tor->info.pieces[piece].hash, SHA_DIGEST_LENGTH);
}
