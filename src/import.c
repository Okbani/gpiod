/*****************************************************************************
 * import.c
 * this file is part of https://github.com/mchalain/gpiod
 *****************************************************************************
 * Copyright (C) 2016-2020
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sched.h>
#include <sys/wait.h>
#include <pthread.h>

#include "log.h"
#include "gpiomonitor.h"
#include "import.h"


typedef struct import_s import_t;
typedef struct import_event_s import_event_t;

struct import_event_s
{
	struct gpiod_line_event event;
	int chipid;
	int gpioid;
	struct import_event_s *next;
};

typedef int (*import_accept_t)(import_t *ctx);
typedef int (*import_scan_t)(const import_t *ctx, int socket, import_event_t *event);
struct import_s
{
	int socket;
	import_accept_t accept;
	import_scan_t scan;
	int rootfd;
	char *url;
	int run;
	import_event_t default_event;
	import_event_t *events;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

};

static int import_scan_raw(const import_t *ctx, int socket, import_event_t *event);
static int import_scan_json(const import_t *ctx, int socket, import_event_t *event);
static int import_scan_simple(const import_t *ctx, int socket, import_event_t *event);

static int import_accept_fifo(import_t *ctx);

static int import_status(import_t *ctx);

static const char str_rising[] = "rising";
static const char str_falling[] = "falling";
static const char str_unamed[] = "unnamed";

static void *import_thread(void * arg)
{
	import_t *ctx = (import_t *)arg;
	while (ctx->run)
	{
		int socket = ctx->accept(ctx);
	}
	return NULL;
}

void *import_create(const char *url, int gpioid, const char *format)
{
	import_t *ctx = calloc(1, sizeof(*ctx));
	ctx->default_event.gpioid = gpioid;
	ctx->default_event.chipid = gpiod_chipid(gpioid);
	dbg("import: import %s", url);
	if (!strncmp(url, "fifo://", 7))
	{
		unlink(url + 7);
		if (mkfifo(url + 7, 0666) == -1)
			err("gpiod: import %s %s", url + 7, strerror(errno));
		ctx->url = strdup(url);
		ctx->accept = &import_accept_fifo;
	}
	// change the scan type
    if (!strcmp(format, "raw"))
	{
		ctx->scan = &import_scan_raw;
	}
	if (!strcmp(format, "simple"))
	{
		ctx->scan = &import_scan_simple;
	}
	ctx->run = 1;
	pthread_cond_init(&ctx->cond, NULL);
	pthread_mutex_init(&ctx->mutex, NULL);
	pthread_create(&ctx->thread, NULL, import_thread, ctx);
	pthread_detach(ctx->thread);
	return ctx;
}

// read socket, parse the buffer
static int import_scan_raw(const import_t *ctx, int socket, import_event_t *event)
{
	event->gpioid = ctx->default_event.gpioid; 
	event->chipid = ctx->default_event.chipid;

    return read(socket, &event->event, sizeof(event->event));
}

static int import_scan_simple(const import_t *ctx, int socket, import_event_t *event)
{
	event->gpioid = ctx->default_event.gpioid; 
	event->chipid = ctx->default_event.chipid;

    char buffer[2];

    int ret = read(socket, buffer, 2);
    dbg("scan %c", buffer[0]);
    
    if (buffer[0] == '1')
    {
        event->event.event_type = GPIOD_LINE_EVENT_RISING_EDGE;
    }
    else
    {
        event->event.event_type = GPIOD_LINE_EVENT_FALLING_EDGE;
    }
    return ret;
}

// static int import_scan_json(const import_t *ctx, int socket, import_event_t *event)
// {
	
//     fscanf()
//     const char *name = gpiod_name(event->gpioid);
// 	if (name == NULL)
// 		name = str_unamed;
// 	int ret;
// 	ret = dprintf(socket,
// 			"{\"chip\" = %.2d; " \
// 			"\"gpio\" = %.2d; " \
// 			"\"name\" = \"%s\"; " \
// 			"\"status\" = \"%s\";}\n",
// 			event->chipid, gpiod_line(event->gpioid), name,
// 			(event->event.event_type == GPIOD_LINE_EVENT_RISING_EDGE)? str_rising: str_falling);
// 	return ret;
// }

static void import_change(import_t *ctx, import_event_t *event)
{
	gpiod_output(event->gpioid, (event->event.event_type == GPIOD_LINE_EVENT_RISING_EDGE));
}

static int import_accept_fifo(import_t *ctx)
{
	/// open block while a writer doesn't open the fifo
	ctx->socket = open(ctx->url + 7, O_RDONLY);
	dbg("import: new fifo client on %s", ctx->url + 7);
	/// only one reader is possible with a fifo
	if (ctx->socket != -1)
	{
        //scan data, create event
        import_event_t event = {0};
        ctx->scan(ctx, ctx->socket, &event);
        import_change(ctx, &event);
            
        close(ctx->socket);
        ctx->socket = -1;
        /// cat try to read several time if closing is too fast
        usleep(200000);
	}
	else
		err("gpiod: open %s %s", ctx->url + 7, strerror(errno));
	return ctx->socket;
}

void import_free(void *arg)
{
	import_t *ctx = (import_t *)arg;
    ctx->run = 0;
	if (ctx->socket >= 0)
		close(ctx->socket);
	pthread_cond_destroy(&ctx->cond);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx->url);
	free(ctx);
}
