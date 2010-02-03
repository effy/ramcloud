/* Copyright (c) 2009 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef RAMCLOUD_LOG_H
#define RAMCLOUD_LOG_H

#include <Common.h>

#include <LogTypes.h>
#include <Segment.h>
#include <BackupClient.h>

namespace RAMCloud {

struct log_entry {
    uint32_t  type;
    uint32_t  length;
};

struct segment_header {
    uint64_t id;
};

struct segment_checksum {
    uint64_t checksum;
};

class LogEntryIterator {
  public:
    explicit LogEntryIterator(const Segment *s);
    bool getNext(const struct log_entry **le, const void **p);
  private:
    const Segment *segment;
    const struct log_entry *next;
    DISALLOW_COPY_AND_ASSIGN(LogEntryIterator);
};

typedef void (*log_eviction_cb_t)(log_entry_type_t, const void *, const uint64_t, void *);
typedef void (*log_entry_cb_t)(log_entry_type_t, const void *, const uint64_t, void *);
typedef void (*log_segment_cb_t)(Segment *, void *);

class Log {
  public:
	Log(const uint64_t, void *, const uint64_t, BackupClient *);
       ~Log();
	const void *append(log_entry_type_t, const void *, uint64_t);
	void        free(log_entry_type_t, const void *, uint64_t);
	bool        registerType(log_entry_type_t, log_eviction_cb_t, void *);
	void        printStats();
	uint64_t    getMaximumAppend();
	void init();
	uint64_t restore();
	void forEachSegment(log_segment_cb_t, uint64_t, void *);
	void forEachEntry(const Segment *, log_entry_cb_t, void *);

  private:
	void        clean(void);
	bool        newHead();
	void        checksumHead();
	void        retireHead();
	const void *appendAnyType(log_entry_type_t, const void *, uint64_t);
	log_eviction_cb_t getEvictionCallback(log_entry_type_t, void **);
	Segment    *getSegment(const void *, uint64_t);

	//XXX- fixme: should be extensible
	log_eviction_cb_t callback;
	log_entry_type_t  callback_type;
	void		 *callback_cookie;

	uint64_t max_append;		// max bytes append() can ever take
	uint64_t segment_size;		// size of each segment in bytes
	void    *base;			// base of all segments
	Segment **segments;		// array of all segments
	Segment *head;			// head of the log
	Segment *free_list;		// free (utilization == 0) segments
	uint64_t nsegments;		// total number of segments in the system
	uint64_t nfree_list;		// number of segments in free list
	uint64_t bytes_stored;		// bytes stored in the log (non-metadata only)
	bool     cleaning;		// presently cleaning the log
	BackupClient *backup;
	DISALLOW_COPY_AND_ASSIGN(Log);
};

} // namespace

#endif // !_LOG_H_