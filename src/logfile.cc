/**
 * Copyright (c) 2007-2012, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file logfile.cc
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/resource.h>

#include <time.h>

#include "base/string_util.hh"
#include "logfile.hh"
#include "lnav_util.hh"

using namespace std;

static const size_t MAX_UNRECOGNIZED_LINES = 1000;
static const size_t INDEX_RESERVE_INCREMENT = 1024;

logfile::logfile(const string &filename, logfile_open_options &loo)
    : lf_filename(filename)
{
    require(!filename.empty());

    memset(&this->lf_stat, 0, sizeof(this->lf_stat));
    if (loo.loo_fd == -1) {
        char resolved_path[PATH_MAX];

        errno = 0;
        if (realpath(filename.c_str(), resolved_path) == NULL) {
            throw error(resolved_path, errno);
        }

        if (stat(resolved_path, &this->lf_stat) == -1) {
            throw error(filename, errno);
        }

        if (!S_ISREG(this->lf_stat.st_mode)) {
            throw error(filename, EINVAL);
        }

        if ((loo.loo_fd = open(resolved_path, O_RDONLY)) == -1) {
            throw error(filename, errno);
        }

        loo.loo_fd.close_on_exec();

        log_info("Creating logfile: fd=%d; size=%" PRId64 "; mtime=%" PRId64 "; filename=%s",
                 (int) loo.loo_fd,
                 (long long) this->lf_stat.st_size,
                 (long long) this->lf_stat.st_mtime,
                 filename.c_str());

        this->lf_valid_filename = true;
    }
    else {
        log_perror(fstat(loo.loo_fd, &this->lf_stat));
        this->lf_valid_filename = false;
    }

    this->lf_content_id = hash_string(this->lf_filename);
    this->lf_line_buffer.set_fd(loo.loo_fd);
    this->lf_index.reserve(INDEX_RESERVE_INCREMENT);

    this->lf_options = loo;

    ensure(this->invariant());
}

logfile::~logfile()
{
}

bool logfile::exists() const
{
    struct stat st;

    if (!this->lf_valid_filename) {
        return true;
    }

    if (::stat(this->lf_filename.c_str(), &st) == -1) {
        return false;
    }

    return this->lf_stat.st_dev == st.st_dev &&
           this->lf_stat.st_ino == st.st_ino &&
           this->lf_stat.st_size <= st.st_size;
}

void logfile::set_format_base_time(log_format *lf)
{
    time_t file_time = this->lf_line_buffer.get_file_time();

    if (file_time == 0) {
        file_time = this->lf_stat.st_mtime;
    }
    lf->lf_date_time.set_base_time(file_time);
}

bool logfile::process_prefix(shared_buffer_ref &sbr, const line_info &li)
{
    log_format::scan_result_t found = log_format::SCAN_NO_MATCH;
    size_t prescan_size = this->lf_index.size();
    time_t prescan_time = 0;
    bool retval = false;

    if (this->lf_format.get() != nullptr) {
        if (!this->lf_index.empty()) {
            prescan_time = this->lf_index[0].get_time();
        }
        /* We've locked onto a format, just use that scanner. */
        found = this->lf_format->scan(*this, this->lf_index, li, sbr);
    }
    else if (this->lf_options.loo_detect_format &&
             this->lf_index.size() < MAX_UNRECOGNIZED_LINES) {
        vector<log_format *> &root_formats =
            log_format::get_root_formats();
        vector<log_format *>::iterator iter;

        /*
         * Try each scanner until we get a match.  Fortunately, all the formats
         * are sufficiently different that there are no ambiguities...
         */
        for (iter = root_formats.begin();
             iter != root_formats.end() && (found != log_format::SCAN_MATCH);
             ++iter) {
            if (!(*iter)->match_name(this->lf_filename)) {
                continue;
            }

            (*iter)->clear();
            this->set_format_base_time(*iter);
            found = (*iter)->scan(*this, this->lf_index, li, sbr);
            if (found == log_format::SCAN_MATCH) {
#if 0
                require(this->lf_index.size() == 1 ||
                       (this->lf_index[this->lf_index.size() - 2] <
                        this->lf_index[this->lf_index.size() - 1]));
#endif
                log_info("%s:%d:log format found -- %s",
                    this->lf_filename.c_str(),
                    this->lf_index.size(),
                    (*iter)->get_name().get());

                this->lf_format = (*iter)->specialized();
                this->set_format_base_time(this->lf_format.get());
                this->lf_content_id = hash_string(string(sbr.get_data(), sbr.length()));

                /*
                 * We'll go ahead and assume that any previous lines were
                 * written out at the same time as the last one, so we need to
                 * go back and update everything.
                 */
                logline &last_line = this->lf_index[this->lf_index.size() - 1];

                for (size_t lpc = 0; lpc < this->lf_index.size() - 1; lpc++) {
                    this->lf_index[lpc].set_time(last_line.get_time());
                    this->lf_index[lpc].set_millis(last_line.get_millis());
                }
                break;
            }
        }
    }

    switch (found) {
        case log_format::SCAN_MATCH:
            if (!this->lf_index.empty()) {
                this->lf_index.back().set_valid_utf(li.li_valid_utf);
            }
            if (!this->lf_index.empty() && prescan_time != this->lf_index[0].get_time()) {
                retval = true;
            }
            if (prescan_size > 0 && prescan_size < this->lf_index.size()) {
                logline &second_to_last = this->lf_index[prescan_size - 1];
                logline &latest = this->lf_index[prescan_size];

                if (latest < second_to_last) {
                    if (this->lf_format->lf_time_ordered) {
                        this->lf_out_of_time_order_count += 1;
                        for (size_t lpc = prescan_size;
                             lpc < this->lf_index.size(); lpc++) {
                            logline &line_to_update = this->lf_index[lpc];

                            line_to_update.set_time_skew(true);
                            line_to_update.set_time(second_to_last.get_time());
                            line_to_update.set_millis(
                                second_to_last.get_millis());
                        }
                    } else {
                        retval = true;
                    }
                }
            }
            break;
        case log_format::SCAN_NO_MATCH: {
            log_level_t last_level = LEVEL_UNKNOWN;
            time_t last_time = this->lf_index_time;
            short last_millis = 0;
            uint8_t last_mod = 0, last_opid = 0;

            if (!this->lf_index.empty()) {
                logline &ll = this->lf_index.back();

                /*
                 * Assume this line is part of the previous one(s) and copy the
                 * metadata over.
                 */
                last_time = ll.get_time();
                last_millis = ll.get_millis();
                if (this->lf_format.get() != NULL) {
                    last_level = (log_level_t)(ll.get_level_and_flags() |
                        LEVEL_CONTINUED);
                }
                last_mod = ll.get_module_id();
                last_opid = ll.get_opid();
            }
            this->lf_index.emplace_back(li.li_file_range.fr_offset,
                                        last_time,
                                        last_millis,
                                        last_level,
                                        last_mod,
                                        last_opid);
            this->lf_index.back().set_valid_utf(li.li_valid_utf);
            break;
        }
        case log_format::SCAN_INCOMPLETE:
            break;
    }

    return retval;
}

logfile::rebuild_result_t logfile::rebuild_index()
{
    rebuild_result_t retval = RR_NO_NEW_LINES;
    struct stat st;

    this->lf_activity.la_polls += 1;

    if (fstat(this->lf_line_buffer.get_fd(), &st) == -1) {
        throw error(this->lf_filename, errno);
    }

    // Check the previous stat against the last to see if things are wonky.
    if (st.st_size < this->lf_stat.st_size ||
        (this->lf_stat.st_size == st.st_size &&
         this->lf_stat.st_mtime != st.st_mtime)) {
        log_info("overwritten file detected, closing -- %s",
                 this->lf_filename.c_str());
        this->close();
        return RR_NO_NEW_LINES;
    }
    else if (this->lf_line_buffer.is_data_available(this->lf_index_size, st.st_size)) {
        this->lf_activity.la_reads += 1;

        // We haven't reached the end of the file.  Note that we use the
        // line buffer's notion of the file size since it may be compressed.
        bool has_format = this->lf_format.get() != nullptr;
        struct rusage begin_rusage;
        off_t off;
        size_t begin_size = this->lf_index.size();
        bool record_rusage = this->lf_index.size() == 1;
        off_t begin_index_size = this->lf_index_size;
        size_t rollback_size = 0;

        if (record_rusage) {
            getrusage(RUSAGE_SELF, &begin_rusage);
        }

        if (!this->lf_index.empty()) {
            off = this->lf_index.back().get_offset();

            /*
             * Drop the last line we read since it might have been a partial
             * read.
             */
            while (this->lf_index.back().get_sub_offset() != 0) {
                this->lf_index.pop_back();
                rollback_size += 1;
            }
            this->lf_index.pop_back();
            rollback_size += 1;

            this->lf_line_buffer.clear();
            if (!this->lf_index.empty()) {
                off_t check_line_off = this->lf_index.back().get_offset();

                auto read_result = this->lf_line_buffer.read_range({
                    check_line_off, this->lf_index_size - check_line_off
                });

                if (read_result.isErr()) {
                    log_info("overwritten file detected, closing -- %s (%s)",
                             this->lf_filename.c_str(),
                             read_result.unwrapErr().c_str());
                    this->close();
                    return RR_INVALID;
                }
            }
        }
        else {
            off = 0;
        }
        if (this->lf_logline_observer != NULL) {
            this->lf_logline_observer->logline_restart(*this, rollback_size);
        }

        bool sort_needed = this->lf_sort_needed;
        this->lf_sort_needed = false;

        auto prev_range = file_range{off};
        while (true) {
            auto load_result = this->lf_line_buffer.load_next_line(prev_range);

            if (load_result.isErr()) {
                this->close();
                return RR_INVALID;
            }

            auto li = load_result.unwrap();

            if (li.li_file_range.empty()) {
                break;
            }
            prev_range = li.li_file_range;

            size_t old_size = this->lf_index.size();

            // Update this early so that line_length() works
            this->lf_index_size = li.li_file_range.next_offset();
            if (old_size == 0) {
                file_range fr = this->lf_line_buffer.get_available();
                auto avail_data = this->lf_line_buffer.read_range(fr);

                this->lf_text_format = avail_data.map(
                    [](const shared_buffer_ref &avail_sbr) -> text_format_t {
                        return detect_text_format(
                            avail_sbr.get_data(), avail_sbr.length());
                    })
                    .unwrapOr(text_format_t::TF_UNKNOWN);
            }

            auto read_result = this->lf_line_buffer.read_range(li.li_file_range);
            if (read_result.isErr()) {
                this->close();
                return RR_INVALID;
            }

            auto sbr = read_result.unwrap().rtrim(is_line_ending);
            this->lf_longest_line = std::max(this->lf_longest_line, sbr.length());
            this->lf_partial_line = li.li_partial;
            sort_needed = this->process_prefix(sbr, li) || sort_needed;

            if (old_size > this->lf_index.size()) {
                old_size = 0;
            }

            for (auto iter = this->begin() + old_size;
                    iter != this->end(); ++iter) {
                if (this->lf_logline_observer != nullptr) {
                    this->lf_logline_observer->logline_new_line(*this, iter, sbr);
                }
            }

            if (this->lf_logfile_observer != nullptr) {
                this->lf_logfile_observer->logfile_indexing(
                    *this,
                    this->lf_line_buffer.get_read_offset(li.li_file_range.next_offset()),
                    st.st_size);
            }

            if (!has_format && this->lf_format != nullptr) {
                break;
            }
        }

        if (this->lf_logline_observer != nullptr) {
            this->lf_logline_observer->logline_eof(*this);
        }

        if (record_rusage && (prev_range.fr_offset - begin_index_size) > (500 * 1024)) {
            struct rusage end_rusage;

            getrusage(RUSAGE_SELF, &end_rusage);
            rusagesub(end_rusage, begin_rusage, this->lf_activity.la_initial_index_rusage);
            log_info("Resource usage for initial indexing of file: %s:%d-%d",
                     this->lf_filename.c_str(),
                     begin_size,
                     this->lf_index.size());
            log_rusage(lnav_log_level_t::INFO, this->lf_activity.la_initial_index_rusage);
        }

        /*
         * The file can still grow between the above fstat and when we're
         * doing the scanning, so use the line buffer's notion of the file
         * size.
         */
        this->lf_index_size = prev_range.next_offset();
        this->lf_stat = st;

        if (sort_needed) {
            retval = RR_NEW_ORDER;
        } else {
            retval = RR_NEW_LINES;
        }
    }

    this->lf_index_time = this->lf_line_buffer.get_file_time();
    if (!this->lf_index_time) {
        this->lf_index_time = st.st_mtime;
    }

    if (this->lf_out_of_time_order_count) {
        log_info("Detected %d out-of-time-order lines in file: %s",
                 this->lf_out_of_time_order_count,
                 this->lf_filename.c_str());
        this->lf_out_of_time_order_count = 0;
    }

    return retval;
}

Result<shared_buffer_ref, std::string> logfile::read_line(logfile::iterator ll)
{
    try {
        return this->lf_line_buffer.read_range(this->get_file_range(ll, false))
            .map([&ll, this](auto sbr) {
                sbr.rtrim(is_line_ending);
                if (!ll->is_valid_utf()) {
                    scrub_to_utf8(sbr.get_writable_data(), sbr.length());
                }

                if (this->lf_format != nullptr) {
                    this->lf_format->get_subline(*ll, sbr);
                }

                return sbr;
            });
    }
    catch (line_buffer::error & e) {
        return Err(string(strerror(e.e_err)));
    }
}

void logfile::read_full_message(logfile::iterator ll,
                                shared_buffer_ref &msg_out,
                                int max_lines)
{
    require(ll->get_sub_offset() == 0);

    size_t line_len = this->line_length(ll);

    try {
        off_t off = ll->get_offset();

        auto read_result = this->lf_line_buffer.read_range({
            off, static_cast<ssize_t>(line_len)});

        if (read_result.isErr()) {
            return;
        }
        msg_out = read_result.unwrap();
        if (this->lf_format.get() != nullptr) {
            this->lf_format->get_subline(*ll, msg_out, true);
        }
    }
    catch (line_buffer::error & e) {
        /* ... */
    }
}

void logfile::set_logline_observer(logline_observer *llo)
{
    this->lf_logline_observer = llo;
    if (llo != nullptr) {
        this->reobserve_from(this->begin());
    }
}

void logfile::reobserve_from(iterator iter)
{
    if (this->lf_logline_observer != NULL) {
        for (; iter != this->end(); ++iter) {
            off_t offset = std::distance(this->begin(), iter);

            if (this->lf_logfile_observer != NULL) {
                this->lf_logfile_observer->logfile_indexing(
                        *this, offset, this->size());
            }

            this->read_line(iter).then([this, iter](auto sbr) {
                this->lf_logline_observer->logline_new_line(*this, iter, sbr);
            });
        }
        if (this->lf_logfile_observer != NULL) {
            this->lf_logfile_observer->logfile_indexing(
                    *this, this->size(), this->size());
        }

        this->lf_logline_observer->logline_eof(*this);
    }
}

filesystem::path logfile::get_path() const
{
    return this->lf_filename;
}

size_t logfile::line_length(logfile::iterator ll, bool include_continues)
{
    iterator next_line = ll;
    size_t retval;

    if (!include_continues && this->lf_next_line_cache) {
        if (ll->get_offset() == (*this->lf_next_line_cache).first) {
            return this->lf_next_line_cache->second;
        }
    }

    do {
        ++next_line;
    } while ((next_line != this->end()) &&
             ((ll->get_offset() == next_line->get_offset()) ||
              (include_continues && next_line->is_continued())));

    if (next_line == this->end()) {
        retval = this->lf_index_size - ll->get_offset();
        if (retval > 0 && !this->lf_partial_line) {
            retval -= 1;
        }
    }
    else {
        retval = next_line->get_offset() - ll->get_offset() - 1;
        if (!include_continues) {
            this->lf_next_line_cache = nonstd::make_optional(
                std::make_pair(ll->get_offset(), retval));
        }
    }

    return retval;
}
