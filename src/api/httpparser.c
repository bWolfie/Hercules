/**
 * This file is part of Hercules.
 * http://herc.ws - http://github.com/HerculesWS/Hercules
 *
 * Copyright (C) 2020 Hercules Dev Team
 *
 * Hercules is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define HERCULES_CORE

#include "config/core.h" // ANTI_MAYAP_CHEAT, RENEWAL, SECURE_NPCTIMEOUT
#include "api/httpparser.h"

#include "common/HPM.h"
#include "common/cbasetypes.h"
#include "common/conf.h"
#include "common/ers.h"
#include "common/grfio.h"
#include "common/memmgr.h"
#include "common/mmo.h" // NEW_CARTS, char_achievements
#include "common/nullpo.h"
#include "common/packets.h"
#include "common/random.h"
#include "common/showmsg.h"
#include "common/socket.h"
#include "common/strlib.h"
#include "common/timer.h"
#include "common/utils.h"
#include "api/aclif.h"
#include "api/apisessiondata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

static struct httpparser_interface httpparser_s;
struct httpparser_interface *httpparser;
static bool isDebug = true;

// parser handlers

#define GET_FD_SD \
	int fd = (int)(intptr_t)parser->data; \
	struct api_session_data *sd = sockt->session[fd]->session_data; \
	nullpo_ret(sd);

#define GET_FD() (int)(intptr_t)parser->data

static int handler_on_message_begin(struct http_parser *parser)
{
	nullpo_ret(parser);
	GET_FD_SD;
	if (sockt->session[fd]->flag.eof)
		return 0;

	sd->flag.message_begin = 1;

	if (isDebug)
		ShowInfo("***MESSAGE BEGIN***\n");
	return 0;
}

static int handler_on_headers_complete(struct http_parser *parser)
{
	nullpo_ret(parser);
	GET_FD_SD;
	if (sockt->session[fd]->flag.eof)
		return 0;

	sd->flag.headers_complete = 1;

	if (isDebug)
		ShowInfo("***HEADERS COMPLETE***\n");
	return 0;
}

static int handler_on_message_complete(struct http_parser *parser)
{
	nullpo_ret(parser);
	GET_FD_SD;

	if (sockt->session[fd]->flag.eof)
		return 0;

	sd->flag.message_complete = 1;
	sd->flag.message_begin = 0;

	if (isDebug)
		ShowInfo("***MESSAGE COMPLETE***\n");
	return 0;
}

static int handler_on_chunk_header(struct http_parser *parser)
{
	nullpo_ret(parser);
	if (isDebug)
		ShowInfo("handler_on_chunk_header\n");
	return 0;
}

static int handler_on_chunk_complete(struct http_parser *parser)
{
	nullpo_ret(parser);
	if (isDebug)
		ShowInfo("handler_on_chunk_complete\n");
	return 0;
}

static int handler_on_url(struct http_parser *parser, const char *at, size_t length)
{
	nullpo_ret(parser);
	nullpo_ret(at);
	int fd = GET_FD();

	if (sockt->session[fd]->flag.eof)
		return 0;

	aclif->set_url(fd, parser->method, at, length);

	if (isDebug) {
		ShowInfo("Url: %d: %.*s\n", parser->method, (int)length, at);
	}
	return 0;
}

static int handler_on_status(struct http_parser *parser, const char *at, size_t length)
{
	nullpo_ret(parser);
	nullpo_ret(at);
	GET_FD_SD;

	if (sockt->session[fd]->flag.eof)
		return 0;

	sd->flag.status = 1;

	if (isDebug)
		ShowInfo("Status: %.*s\n", (int)length, at);
	return 0;
}

static int handler_on_header_field(struct http_parser *parser, const char *at, size_t length)
{
	nullpo_ret(parser);
	nullpo_ret(at);
	GET_FD_SD;

	if (sockt->session[fd]->flag.eof)
		return 0;

	if (isDebug)
		ShowInfo("Header field: %.*s\n", (int)length, at);

	aclif->set_header_name(fd, at, length);
	return 0;
}

static int handler_on_header_value(struct http_parser *parser, const char *at, size_t length)
{
	nullpo_ret(parser);
	nullpo_ret(at);
	GET_FD_SD;

	if (sockt->session[fd]->flag.eof)
		return 0;

	if (isDebug)
		ShowInfo("Header value: %.*s\n", (int)length, at);

	aclif->set_header_value(fd, at, length);
	return 0;
}

static int handler_on_body(struct http_parser *parser, const char *at, size_t length)
{
	nullpo_ret(parser);
	nullpo_ret(at);
	GET_FD_SD;

	if (sockt->session[fd]->flag.eof)
		return 0;

	sd->flag.body = 1;

	if (isDebug) {
		ShowInfo("Body: %.*s\n", (int)length, at);
		ShowInfo("end body\n");
	}
	return 0;
}


static bool httpparser_parse(int fd)
{
	nullpo_ret(sockt->session[fd]);

	struct api_session_data *sd = sockt->session[fd]->session_data;
	size_t data_size = RFIFOREST(fd);
	size_t parsed_size = http_parser_execute(&sd->parser, httpparser->settings, RFIFOP(fd, 0), data_size);
	RFIFOSKIP(fd, data_size);
	return data_size == parsed_size;
}

static void httpparser_init_parser(int fd, struct api_session_data *sd)
{
	nullpo_retv(sd);
	http_parser_init(&sd->parser, HTTP_REQUEST);
	sd->parser.data = (void*)(intptr_t)fd;
}

static void httpparser_delete_parser(int fd)
{
}

static int do_init_httpparser(bool minimal)
{
	if (minimal)
		return 0;

	httpparser->settings = aCalloc(1, sizeof(struct http_parser_settings));
	httpparser->settings->on_message_begin = httpparser->on_message_begin;
	httpparser->settings->on_url = httpparser->on_url;
	httpparser->settings->on_header_field = httpparser->on_header_field;
	httpparser->settings->on_header_value = httpparser->on_header_value;
	httpparser->settings->on_headers_complete = httpparser->on_headers_complete;
	httpparser->settings->on_body = httpparser->on_body;
	httpparser->settings->on_message_complete = httpparser->on_message_complete;
	httpparser->settings->on_status = httpparser->on_status;
	httpparser->settings->on_chunk_header = httpparser->on_chunk_header;
	httpparser->settings->on_chunk_complete = httpparser->on_chunk_complete;

	return 0;
}

static void do_final_httpparser(void)
{
	aFree(httpparser->settings);
}

void httpparser_defaults(void)
{
	httpparser = &httpparser_s;
	/* vars */
	httpparser->settings = NULL;
	/* core */
	httpparser->init = do_init_httpparser;
	httpparser->final = do_final_httpparser;
	httpparser->parse = httpparser_parse;
	httpparser->init_parser = httpparser_init_parser;
	httpparser->delete_parser = httpparser_delete_parser;

	httpparser->on_message_begin = handler_on_message_begin;
	httpparser->on_url = handler_on_url;
	httpparser->on_header_field = handler_on_header_field;
	httpparser->on_header_value = handler_on_header_value;
	httpparser->on_headers_complete = handler_on_headers_complete;
	httpparser->on_body = handler_on_body;
	httpparser->on_message_complete = handler_on_message_complete;
	httpparser->on_status = handler_on_status;
	httpparser->on_chunk_header = handler_on_chunk_header;
	httpparser->on_chunk_complete = handler_on_chunk_complete;
}
