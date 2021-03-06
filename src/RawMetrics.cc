/* Copyright (c) 2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for
 * any purpose with or without fee is hereby granted, provided that
 * the above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
 * AUTHORS BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Common.h"
#include "Cycles.h"
#include "ShortMacros.h"
#include "RawMetrics.h"
#include "MetricList.pb.h"
#include "Segment.h"

namespace RAMCloud {

namespace {
    /// See #metrics.
    RawMetrics _metrics;
};

/**
 * Stores recovery metrics.
 * This is a pointer for future expansion. It always points to the same RawMetrics
 * object now.
 */
RawMetrics* metrics = &_metrics;

/**
 * This method is invoked from the constructor (which is defined in
 * RawMetrics.in.h).  It initializes a few special "metrics" that contain
 * general information about the server.
 */
void
RawMetrics::init()
{
    Cycles::init();
    clockFrequency = (uint64_t) Cycles::perSecond();
    pid = getpid();
    segmentSize = Segment::DEFAULT_SEGMENT_SIZE;
}

/**
 * Generate a string that contains a serialized representation of all of the
 * performance counters.
 *
 * \param out
 *      The contents of this variable are replaced with a (binary) string
 *      formatted using Protocol Buffers and MetricList.proto.
 */
void
RawMetrics::serialize(std::string& out)
{
     ProtoBuf::MetricList list;
     for (int i = 0; i < numMetrics; i++) {
        MetricInfo info = metricInfo(i);
        ProtoBuf::MetricList_Entry* metric = list.add_metric();
        metric->set_name(info.name);
        metric->set_value(*info.value);
     }
     out.clear();
     list.SerializeToString(&out);
}

}  // namespace RAMCloud

// This file is automatically generated from scripts/rawmetrics.py; it defines
// the metricInfo method.
#include "RawMetrics.in.cc"
