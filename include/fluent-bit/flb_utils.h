/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2017 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef FLB_UTILS_H
#define FLB_UTILS_H

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_pipe.h>
#include <fluent-bit/flb_config.h>

struct flb_split_entry {
    char *value;
    int len;
    struct mk_list _head;
};

void flb_utils_error(int err);
void flb_utils_error_c(const char *msg);
void flb_utils_warn_c(const char *msg);
void flb_message(int type, char *file, int line, const char *fmt, ...);

#ifdef FLB_HAVE_FORK
int flb_utils_set_daemon();
#endif

void flb_utils_print_setup(struct flb_config *config);

struct mk_list *flb_utils_split(char *line, int separator, int max_split);

void flb_utils_split_free(struct mk_list *list);
int flb_utils_timer_consume(flb_pipefd_t fd);
size_t flb_utils_size_to_bytes(char *size);
int flb_utils_time_to_seconds(char *time);
int flb_utils_pipe_byte_consume(flb_pipefd_t fd);
int flb_utils_bool(char *val);

#endif
