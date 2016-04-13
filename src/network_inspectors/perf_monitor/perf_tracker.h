//--------------------------------------------------------------------------
// Copyright (C) 2015-2016 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// perf_tracker.h author Carter Waxman <cwaxman@cisco.com>

//
// This class defines the data gathering layer of perfmon. PerfMonitor will
// create an instance of each configued class for each packet processing
// thread. Subclasses of PerfTrackers should implement or call the following
// methods, leaving the others for internal use by PerfMonitor:
// 
// reset() - perform initialization after the output handle has been opened.
//
// update(Packet*) - update statistics basied on the current packet.
//
// process(bool) - summarize data and report. This is called after the
// reporting thresholds have been reached.
//

#ifndef PERF_TRACKER_H
#define PERF_TRACKER_H

#include <cstdio>

#include "perf_formatter.h"
#include "perf_monitor.h"

class PerfTracker
{
public:
    virtual void reset() {}

    virtual void update(Packet*) {};
    virtual void process(bool /*summary*/) {}; //FIXIT-M get rid of this step.

    virtual void update_time(time_t time) final { cur_time = time; };
    virtual void open(bool append) final;
    virtual void close() final;
    virtual void rotate() final;
    virtual void auto_rotate() final;

    virtual ~PerfTracker();

protected:
    PerfConfig* config;
    FILE* fh = nullptr;
    std::string fname;
    time_t cur_time;
    PerfFormatter* formatter;

    PerfTracker(PerfConfig*, const char* tracker_fname);

private:
    
};
#endif

