// =====================================================================
//
// TOD_clock.cxx
//
// Copyright (C) 2017
//		Dave Freese, W1HKJ
//
// This file is part of flrig
//
// Fldigi is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Fldigi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with fldigi.  If not, see <http://www.gnu.org/licenses/>.
// =====================================================================

#include "config.h"

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>

#include <cmath>
#include <cstring>

#include <iostream>

#include <FL/Fl.H>

#include "util.h"
#include "debug.h"
//#include "status.h"
#include "icons.h"

#include "tod_clock.h"
#include "timeops.h"

// [subspace5] These helpers are called from the serial loop, the XML-RPC
// server threads and the GUI at the same time. The timeval, tm and text
// buffer used to be ONE static each, shared by every thread: a second
// caller could overwrite tv between gettimeofday() and the arithmetic
// (zmsec, zusec: a torn millisecond count feeding the cache-staleness
// tests in xml_server.h) or overwrite the text while another thread was
// still streaming it (ztime). Scratch values are now locals; the text
// buffer ztime() returns is one per thread, so the returned pointer stays
// valid for the immediate use every caller makes of it.
ullint zmsec()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000LL + (tv.tv_usec / 1000LL));
}

ullint zusec()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000000LL + tv.tv_usec);
}

char *ztime()
{
	static thread_local char exttime[13];

	struct tm tim;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	time_t tval = tv.tv_sec;

	gmtime_r(&tval, &tim);

	snprintf(exttime, sizeof(exttime),
		"%02d:%02d:%02d.%03d",
		tim.tm_hour, tim.tm_min, tim.tm_sec, (int)(tv.tv_usec / 1000L) );

	return exttime;
}
