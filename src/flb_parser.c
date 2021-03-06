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

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_log.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/flb_str.h>
#include <fluent-bit/flb_parser.h>
#include <fluent-bit/flb_time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>

int flb_parser_regex_do(struct flb_parser *parser,
                        char *buf, size_t length,
                        void **out_buf, size_t *out_size,
                        struct flb_time *out_time);

int flb_parser_json_do(struct flb_parser *parser,
                       char *buf, size_t length,
                       void **out_buf, size_t *out_size,
                       struct flb_time *out_time);

struct flb_parser *flb_parser_create(char *name, char *format,
                                     char *p_regex,
                                     char *time_fmt, char *time_key,
                                     int time_keep, struct flb_config *config)
{
    int size;
    char *tmp;
    struct mk_list *head;
    struct flb_parser *p;
    struct flb_regex *regex;

    /* Iterate current parsers and make the new one don't exists */
    mk_list_foreach(head, &config->parsers) {
        p = mk_list_entry(head, struct flb_parser, _head);
        if (strcmp(p->name, name) == 0) {
            flb_error("[parser] parser named '%s' already exists, skip.",
                      name);
            return NULL;
        }
    }

    p = flb_calloc(1, sizeof(struct flb_parser));
    if (!p) {
        flb_errno();
        return NULL;
    }

    /* Format lookup */
    if (strcmp(format, "regex") == 0) {
        p->type = FLB_PARSER_REGEX;
    }
    else if (strcmp(format, "json") == 0) {
        p->type = FLB_PARSER_JSON;
    }
    else {
        fprintf(stderr, "[parser] Invalid format %s\n", format);
        flb_free(p);
        return NULL;
    }

    if (p->type == FLB_PARSER_REGEX) {
        if (!p_regex) {
            fprintf(stderr, "[parser] Invalid regex pattern\n");
            flb_free(p);
            return NULL;
        }

        regex = flb_regex_create((unsigned char *) p_regex);
        if (!regex) {
            fprintf(stderr, "[parser] Invalid regex pattern %s\n", p_regex);
            flb_free(p);
            return NULL;
        }
        p->regex = regex;
        p->p_regex = flb_strdup(p_regex);
    }

    p->name = flb_strdup(name);

    if (time_fmt) {
        p->time_fmt = flb_strdup(time_fmt);

        /* Check if the format is considering the year */
        if (strstr(p->time_fmt, "%Y") || strstr(p->time_fmt, "%y")) {
            p->time_with_year = FLB_TRUE;
        }
        else {
            size = strlen(p->time_fmt);
            p->time_with_year = FLB_FALSE;
            p->time_fmt_year = flb_malloc(size + 4);
            if (!p->time_fmt_year) {
                flb_errno();
                flb_parser_destroy(p);
                return NULL;
            }

            memcpy(p->time_fmt_year, p->time_fmt, size);
            tmp = p->time_fmt_year + size;
            *tmp++ = ' ';
            *tmp++ = '%';
            *tmp++ = 'Y';
            *tmp++ = '\0';
        }

        /*
         * Check if the format expect fractional seconds
         *
         * Since strptime(3) does not support fractional seconds, this
         * requires a workaround/hack in our parser. This is a known
         * issue and addressed in different ways in other languages.
         *
         * The following links are a good reference:
         *
         * - http://stackoverflow.com/questions/7114690/how-to-parse-syslog-timestamp
         * - http://code.activestate.com/lists/python-list/521885/
         */
        tmp = strstr(p->time_fmt, "%S.%L");
        if (tmp) {
            tmp[2] = '\0';
            p->time_frac_secs = (tmp + 3);
        }
        else {
            p->time_frac_secs = NULL;
        }
    }

    if (time_key) {
        p->time_key = flb_strdup(time_key);
    }
    p->time_keep = time_keep;

    mk_list_add(&p->_head, &config->parsers);

    return p;
}

void flb_parser_destroy(struct flb_parser *parser)
{
    if (parser->type == FLB_PARSER_REGEX) {
        flb_regex_destroy(parser->regex);
        flb_free(parser->p_regex);
    }

    flb_free(parser->name);
    if (parser->time_fmt) {
        flb_free(parser->time_fmt);
    }
    if (parser->time_fmt_year) {
        flb_free(parser->time_fmt_year);
    }
    if (parser->time_key) {
        flb_free(parser->time_key);
    }

    mk_list_del(&parser->_head);
    flb_free(parser);
}

void flb_parser_exit(struct flb_config *config)
{
    struct mk_list *tmp;
    struct mk_list *head;
    struct flb_parser *parser;

    mk_list_foreach_safe(head, tmp, &config->parsers) {
        parser = mk_list_entry(head, struct flb_parser, _head);
        flb_parser_destroy(parser);
    }
}

/* Load parsers from a configuration file */
int flb_parser_conf_file(char *file, struct flb_config *config)
{
    int ret;
    char tmp[PATH_MAX + 1];
    char *cfg = NULL;
    char *name;
    char *format;
    char *regex;
    char *time_fmt;
    char *time_key;
    int time_keep;
    struct mk_rconf *fconf;
    struct mk_rconf_section *section;
    struct mk_list *head;
    struct stat st;

    ret = stat(file, &st);
    if (ret == -1 && errno == ENOENT) {
        /* Try to resolve the real path (if exists) */
        if (file[0] == '/') {
            return -1;
        }

        if (config->conf_path) {
            snprintf(tmp, PATH_MAX, "%s%s", config->conf_path, file);
            cfg = tmp;
        }
    }
    else {
        cfg = file;
    }

    flb_debug("[parser] opening file %s", cfg);
    fconf = mk_rconf_open(cfg);
    if (!fconf) {
        return -1;
    }

    /* Read all [PARSER] sections */
    mk_list_foreach(head, &fconf->sections) {
        name = NULL;
        format = NULL;
        regex = NULL;
        time_fmt = NULL;
        time_key = NULL;

        section = mk_list_entry(head, struct mk_rconf_section, _head);
        if (strcasecmp(section->name, "PARSER") != 0) {
            continue;
        }

        /* Name */
        name = mk_rconf_section_get_key(section, "Name", MK_RCONF_STR);
        if (!name) {
            flb_error("[parser] no parser 'name' found");
            goto fconf_error;
        }

        /* Format */
        format = mk_rconf_section_get_key(section, "Format", MK_RCONF_STR);
        if (!format) {
            flb_error("[parser] no parser 'format' found");
            goto fconf_error;
        }

        /* Regex (if format is regex) */
        regex = mk_rconf_section_get_key(section, "Regex", MK_RCONF_STR);
        if (!regex && strcmp(format, "regex") == 0) {
            flb_error("[parser] no parser 'regex' found");
            goto fconf_error;
        }

        /* Time_Format */
        time_fmt = mk_rconf_section_get_key(section, "Time_Format",
                                            MK_RCONF_STR);

        /* Time_Key */
        time_key = mk_rconf_section_get_key(section, "Time_Key",
                                            MK_RCONF_STR);

        /* Time_Keep */
        time_keep = (intptr_t) mk_rconf_section_get_key(section, "Time_Keep",
                                                        MK_RCONF_BOOL);
        if (time_keep < 0) {
            flb_error("[parser] Invalid time_keep value (try On/Off)");
            goto fconf_error;
        }

        /* Create the parser context */
        if (!flb_parser_create(name, format, regex,
                               time_fmt, time_key, time_keep, config)) {
            goto fconf_error;
        }

        flb_debug("[parser] new parser registered: %s", name);

        flb_free(name);
        flb_free(format);

        if (regex) {
            flb_free(regex);
        }
        if (time_fmt) {
            flb_free(time_fmt);
        }
        if (time_key) {
            flb_free(time_key);
        }
    }

    mk_rconf_free(fconf);
    return 0;

 fconf_error:
    mk_rconf_free(fconf);
    return -1;
}

struct flb_parser *flb_parser_get(char *name, struct flb_config *config)
{
    struct mk_list *head;
    struct flb_parser *parser;


    mk_list_foreach(head, &config->parsers) {
        parser = mk_list_entry(head, struct flb_parser, _head);
        if (strcmp(parser->name, name) == 0) {
            return parser;
        }
    }

    return NULL;
}

int flb_parser_do(struct flb_parser *parser, char *buf, size_t length,
                  void **out_buf, size_t *out_size, struct flb_time *out_time)
{

    if (parser->type == FLB_PARSER_REGEX) {
        return flb_parser_regex_do(parser, buf, length,
                                   out_buf, out_size, out_time);
    }
    else if (parser->type == FLB_PARSER_JSON) {
        return flb_parser_json_do(parser, buf, length,
                                  out_buf, out_size, out_time);
    }

    return -1;
}


int flb_parser_frac_tzone(char *str, int len, double *frac, int *tmdiff)
{
    int neg;
    long hour;
    long min;
    char *p;
    char *end;

    /* Fractional seconds */
    *frac = strtod(str, &end);
    p = end;

    if (!p) {
        *tmdiff = 0;
        return 0;
    }

    while (*p == ' ') p++;

    /* Check timezones */
    if (*p == 'Z') {
        /* This is UTC, no changes required */
        *tmdiff = 0;
        return 0;
    }

    /* Unexpected timezone string */
    if (*p != '+' && *p != '-') {
        *tmdiff = 0;
        return 0;
    }

    /* Negative value ? */
    neg = (*p++ == '-');

    /* Locate end */
    end = str + len;

    /* Gather hours and minutes */
    hour = ((p[0] - '0') * 10) + (p[1] - '0');
    if (end - p == 5 && p[2] == ':') {
        min = ((p[3] - '0') * 10) + (p[4] - '0');
    }
    else {
        min = ((p[2] - '0') * 10) + (p[3] - '0');
    }

    if (hour < 0 || hour > 59 || min < 0 || min > 59) {
        return -1;
    }

    *tmdiff = ((hour * 3600) + (min * 60));
    if (neg) {
        *tmdiff = -*tmdiff;
    }

    return 0;
}
