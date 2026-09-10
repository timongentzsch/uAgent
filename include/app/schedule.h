// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_SCHEDULE_H_
#define UAGENT_INCLUDE_APP_SCHEDULE_H_
#include <cstdint>
#include <functional>
#include <string>

#include "include/core/json.h"
namespace uagent {
std::string SchedulePath();
json ScheduleControl(const json& request);
// Only the isolated --control helper calls this: libc timezone state is
// process-global.
json ScheduleCalendar(const json& request);
json ScheduleTimes(const json& schedule, int64_t after);
json ReadSchedules();
json ClaimScheduledRuns(int64_t now, size_t slots);
json UpdateScheduledRun(const std::string& id, const std::string& status,
                        const std::string& error = "");
json RecoverScheduledRuns();
bool ScheduledRunActive(const std::string& status);
}  // namespace uagent
#endif
