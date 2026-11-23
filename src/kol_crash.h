// kol_crash.h — a log a player can find and send: the session's output and,
// when the game crashes, what happened (2026-11-23).
//
// Each run writes <logs>/last_run.log; the one before is kept as
// previous_run.log. A crash appends a report to it (the signal or exception,
// the code address and its library, a backtrace, the guest's PC and frame),
// then the system's own crash handling runs as before (its report, the exit
// status). Desktop: <data folder>/logs, opened by Options > Technical > Logs.
// Android: files/logs, shown in the Files app under the game's name.
#pragma once

#include <cstddef>
#include <string>

namespace kol {

// Once, early: the session's log in `logs_dir`. `redirect_stdio`: stdout and
// stderr go into it (a public desktop build, whose output nothing shows);
// else it holds the crash report only (and what crash_log_write adds).
void crash_log_begin(const std::string& logs_dir, bool redirect_stdio);

// Text for the log as it is (Android: the output's pump, which also feeds
// logcat). Any thread.
void crash_log_write(const char* text, std::size_t n);

// Once, early: the crash handlers (fatal signals, unhandled exceptions,
// std::terminate). Before crash_log_begin the report goes to stderr.
void crash_handlers_install();

// The logs folder (empty before crash_log_begin).
std::string crash_logs_dir();

// The run before this one ended in a crash (its log holds a report).
bool crash_log_previous_crashed();

// Every frame (the frame hook): after a crash, once, a second into the
// session, a notice saying where its log is.
void crash_notice_frame(unsigned long long frame_count);

}  // namespace kol
