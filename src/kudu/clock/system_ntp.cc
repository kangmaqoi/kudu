// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/clock/system_ntp.h"

#include <sys/time.h>
#include <sys/timex.h>

#include <cerrno>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

#include <gflags/gflags_declare.h>
#include <glog/logging.h>

#include "kudu/gutil/port.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/errno.h"
#include "kudu/util/logging.h"
#include "kudu/util/path_util.h"
#include "kudu/util/status.h"
#include "kudu/util/subprocess.h"

DECLARE_bool(inject_unsync_time_errors);

using std::string;
using std::vector;
using strings::Substitute;

namespace kudu {
namespace clock {

namespace {

Status NtpStateToStatus(int rc) {
  switch (rc) {
    case TIME_OK:
      return Status::OK();
    case -1: // generic error
      {
        int err = errno;
        // From 'man 2 adjtimex', ntp_adjtime failure implies an improper 'tx'.
        return Status::InvalidArgument(
            "Error reading clock. ntp_adjtime() failed", ErrnoToString(err));
      }
    case TIME_ERROR:
      return Status::ServiceUnavailable(
          PREDICT_FALSE(FLAGS_inject_unsync_time_errors) ?
          "Injected clock unsync error" :
          "Error reading clock. Clock considered unsynchronized");
    default:
      // TODO(dralves): what to do about leap seconds? see KUDU-146
      KLOG_FIRST_N(ERROR, 1)
          << "Server undergoing leap second. This may cause consistency issues "
          << "(rc=" << rc << ")";
      return Status::OK();
  }
}

void TryRun(vector<string> cmd, vector<string>* log) {
  string exe;
  Status s = FindExecutable(cmd[0], {"/sbin", "/usr/sbin/"}, &exe);
  if (!s.ok()) {
    LOG_STRING(WARNING, log) << "could not find executable: " << cmd[0];
    return;
  }

  cmd[0] = exe;
  string out;
  string err;
  s = Subprocess::Call(cmd, "", &out, &err);
  // Subprocess::Call() returns RuntimeError in the case that the process returns
  // a non-zero exit code, but that might still generate useful err.
  if (s.ok() || (s.IsRuntimeError() && (!out.empty() || !err.empty()))) {
    LOG_STRING(ERROR, log)
        << JoinStrings(cmd, " ")
        << "\n------------------------------------------"
        << (!out.empty() ? Substitute("\nstdout:\n$0", out) : "")
        << (!err.empty() ? Substitute("\nstderr:\n$0", err) : "")
        << "\n";
  } else {
    LOG_STRING(WARNING, log) << "failed to run executable: " << cmd[0];
  }
}

} // anonymous namespace

SystemNtp::SystemNtp()
    : skew_ppm_(std::numeric_limits<int64_t>::max()) {
}

Status SystemNtp::Init() {
  timex tx;
  tx.modes = 0; // set mode to 0 for read-only query
  RETURN_NOT_OK(NtpStateToStatus(ntp_adjtime(&tx)));
  // The unit of the reported tolerance is ppm with 16-bit fractional part:
  // 65536 is 1 ppm (see http://man7.org/linux/man-pages/man3/ntp_adjtime.3.html
  // for details).
  skew_ppm_ = tx.tolerance / 65536;
  return Status::OK();
}

Status SystemNtp::WalltimeWithError(uint64_t* now_usec, uint64_t* error_usec) {
  if (PREDICT_FALSE(FLAGS_inject_unsync_time_errors)) {
    return NtpStateToStatus(TIME_ERROR);
  }
  // Read the time. This will return an error if the clock is not synchronized.
  ntptimeval tv;
  const int rc = ntp_gettime(&tv);
  // ntp_gettime() never fails according to its manual page.
  PCHECK(rc != -1);
  RETURN_NOT_OK(NtpStateToStatus(rc));
  uint64_t now = static_cast<uint64_t>(tv.time.tv_sec) * 1000000;
#ifdef __APPLE__
  now += tv.time.tv_nsec / 1000;
#else
  now += tv.time.tv_usec;
#endif
  *now_usec = now;
  *error_usec = tv.maxerror;
  return Status::OK();
}

void SystemNtp::DumpDiagnostics(vector<string>* log) const {
  LOG_STRING(ERROR, log) << "Dumping NTP diagnostics";
  TryRun({"ntptime"}, log);
  // Gather as much info as possible from both ntpq and ntpdc, even
  // though some of it might be redundant. Different versions of ntp
  // expose different sets of commands through these two tools.
  // The tools will happily ignore commmands they don't understand.
  TryRun({"ntpq", "-n",
          "-c", "timeout 1000",
          "-c", "readvar",
          "-c", "sysinfo",
          "-c", "lpeers",
          "-c", "opeers",
          "-c", "version"}, log);
  TryRun({"ntpdc", "-n",
          "-c", "timeout 1000",
          "-c", "peers",
          "-c", "sysinfo",
          "-c", "sysstats",
          "-c", "version"}, log);

  TryRun({"chronyc", "-n", "tracking"}, log);
  TryRun({"chronyc", "-n", "sources"}, log);
}

} // namespace clock
} // namespace kudu
