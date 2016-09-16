/*
 * Copyright (c) 2016, Mark Jones <markalanj@gmail.com>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * The names of its contributors may not be used to endorse or promote
 * products derived from this software without specific prior written
 * permission.

 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <erl_driver.h>
#include <ei.h>
#include <erl_interface.h>
#include <linux/time.h>
#include <sys/timerfd.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include "logger.h"
#include "ei_x_extras.h"

#define MODULE          "etimerfd"
#define LOGFILE         "etimerfd.log"

#define ATOM_OK         "ok"
#define ATOM_ERROR      "error"
#define ATOM_NOMEMORY   "no_memory"

#define EVENT2FD(x) (long)(x)
#define FD2EVENT(x) (ErlDrvEvent)(long)(x)

typedef struct
{
    ErlDrvPort port;
    int fd;
    bool ack_pending;
} etimerfd;

enum
{
    CREATE  = 0,
    SETTIME = 1,
    GETTIME = 2,
    ACK = 3
};

#if DEBUG > 0
static void dump_eterm_statistics()
{
    unsigned long allocated, freed;

    erl_eterm_statistics(&allocated, &freed);
    LOGGER_PRINT("currently allocated blocks: %ld", allocated);
    LOGGER_PRINT("length of freelist: %ld", freed);

    /* really free the freelist */
    erl_eterm_release();
}
#endif

static ErlDrvSSizeT encode_error(ei_x_buff *x_buff, const char *str)
{
    ei_x_encode_tuple_header(x_buff, 2);
    ei_x_encode_atom(x_buff, ATOM_ERROR);
    ei_x_encode_string(x_buff, str);
    return x_buff->index;
}

static ErlDrvSSizeT create_timer(etimerfd *data, ei_x_buff *in_x_buff,
                                 ei_x_buff *out_x_buff)
{
    char atom[MAXATOMLEN];
    int clockid = -1;

    if(ei_x_decode_atom(in_x_buff, atom) == 0)
    {
        if(strcmp(atom, "clock_monotonic") == 0)
            clockid = CLOCK_MONOTONIC;
        else if(strcmp(atom, "clock_realtime") == 0)
            clockid = CLOCK_REALTIME;

        if(clockid != -1)
        {
            data->fd = timerfd_create(clockid, TFD_NONBLOCK | TFD_CLOEXEC);
            if(data->fd < 0)
            {
                LOGGER_PRINT("timerfd_create() failed");
                encode_error(out_x_buff, "timerfd_create failed");
            }
            else
            {
                LOGGER_PRINT("timerfd_create() success");
                driver_select(data->port, FD2EVENT(data->fd),
                              ERL_DRV_READ | ERL_DRV_USE, 1);
                ei_x_encode_atom(out_x_buff, ATOM_OK);
            }
        }
        else
        {
            LOGGER_PRINT("%s is bad clockid", atom);
            return -1; /* "Let it crash" */
        }
    }

    return out_x_buff->index;
}

static ErlDrvSSizeT settime(etimerfd *data, ei_x_buff *in_x_buff,
                            ei_x_buff *out_x_buff)
{
    ETERM *term, *pattern, *reply;
    struct itimerspec new_value, old_value;
    int flags = 0;

    ei_decode_term(in_x_buff->buff, &in_x_buff->index, &term);
    if(term == NULL)
        return -1;

    pattern = erl_format("{{{A,B},{C,D}},E}");
    if(erl_match(pattern, term))
    {
        new_value.it_interval.tv_sec = ERL_INT_UVALUE(
                                           erl_var_content(pattern, "A"));
        new_value.it_interval.tv_nsec = ERL_INT_UVALUE(
                                            erl_var_content(pattern, "B"));
        new_value.it_value.tv_sec = ERL_INT_UVALUE(
                                        erl_var_content(pattern, "C"));
        new_value.it_value.tv_nsec = ERL_INT_UVALUE(
                                         erl_var_content(pattern, "D"));

        if(strcmp(ERL_ATOM_PTR(erl_var_content(pattern, "E")), "true") == 0)
            flags = TFD_TIMER_ABSTIME;

        if(timerfd_settime(data->fd, flags, &new_value, &old_value) == 0)
        {
            LOGGER_PRINT("timerfd_settime sucessful");
            reply = erl_format("{~a,{{~i,~i},{~i,~i}}}",
                               ATOM_OK,
                               old_value.it_interval.tv_sec,
                               old_value.it_interval.tv_nsec,
                               old_value.it_value.tv_sec,
                               old_value.it_value.tv_nsec);
            ei_x_encode_term(out_x_buff, reply);
            erl_free_compound(reply);
        }
    }
    erl_free_compound(pattern);
    erl_free_compound(term);

    return out_x_buff->index;
}

static ErlDrvSSizeT gettime(etimerfd *data, ei_x_buff *in_x_buff,
                            ei_x_buff *out_x_buff)
{
    struct itimerspec curr_value;
    ETERM *reply;

    if(timerfd_gettime(data->fd, &curr_value) == 0)
    {
        LOGGER_PRINT("timerfd_gettime");
        reply = erl_format("{~a,{{~i,~i},{~i,~i}}}",
                           ATOM_OK,
                           curr_value.it_interval.tv_sec,
                           curr_value.it_interval.tv_nsec,
                           curr_value.it_value.tv_sec,
                           curr_value.it_value.tv_nsec);
        ei_x_encode_term(out_x_buff, reply);
        erl_free_compound(reply);
    }

    return out_x_buff->index;
}

static ErlDrvSSizeT ack(etimerfd *data, ei_x_buff *in_x_buff,
                        ei_x_buff *out_x_buff)
{
    if(data->ack_pending)
    {
        LOGGER_PRINT("timeout acknowlaged");
        data->ack_pending = false;
        driver_select(data->port, FD2EVENT(data->fd),
                      ERL_DRV_READ | ERL_DRV_USE, 1);
        ei_x_encode_atom(out_x_buff, ATOM_OK);
    }
    else
    {
        LOGGER_PRINT("ack not pending");
        encode_error(out_x_buff, "ack not pending");
    }
    return out_x_buff->index;
}

static int init(void)
{
    erl_init(NULL, 0);
    LOGGER_OPEN(MODULE, LOGFILE);
    LOGGER_PRINT("driver loaded");
    return 0;
}

static void finish(void)
{
    LOGGER_PRINT("driver unloaded");
    LOGGER_CLOSE();
}

static ErlDrvData start(ErlDrvPort port, char *cmd)
{
    etimerfd *data = (etimerfd *)driver_alloc(sizeof(etimerfd));
    if(data)
    {
        set_port_control_flags(port, PORT_CONTROL_FLAG_BINARY);
        data->port = port;
        data->fd = -1;
        data->ack_pending = false;
        LOGGER_PRINT("port opened");
    }
    else
    {
        LOGGER_PRINT("failed to allocate port data");
        driver_failure_atom(port, ATOM_NOMEMORY);
    }
    return (ErlDrvData)data;
}

static void stop(ErlDrvData handle)
{
    etimerfd *data = (etimerfd *)handle;

    if(data->fd)
    {
        driver_select(data->port, FD2EVENT(data->fd), ERL_DRV_READ, 0);
        close(data->fd);
    }

    driver_free(data);
    LOGGER_PRINT("port closed");
}

static ErlDrvSSizeT control(ErlDrvData handle,
                            unsigned int command,
                            char *buf, ErlDrvSizeT len,
                            char **rbuf, ErlDrvSizeT rlen)
{
    etimerfd *data = (etimerfd *)handle;
    ei_x_buff in_x_buff = {buf, len, 0};
    ei_x_buff out_x_buff;
    int version;
    ErlDrvSSizeT tmp;

    ei_x_decode_version(&in_x_buff, &version);

    if(ei_x_new_with_version(&out_x_buff) != 0)
    {
        driver_failure_atom(data->port, ATOM_NOMEMORY);
        return 0;
    }

    switch(command)
    {
    case CREATE:
        tmp = create_timer(data, &in_x_buff, &out_x_buff);
        break;

    case SETTIME:
        tmp = settime(data, &in_x_buff, &out_x_buff);
        break;

    case GETTIME:
        tmp = gettime(data, &in_x_buff, &out_x_buff);
        break;

    case ACK:
        tmp = ack(data, &in_x_buff, &out_x_buff);
        break;

    default:
        tmp = -1; /* "Let it crash" */
        break;
    }

    *rbuf = (char *)ei_x_to_new_binary(&out_x_buff);
    ei_x_free(&out_x_buff);

    return tmp;
}

static void ready_input(ErlDrvData handle, ErlDrvEvent event)
{
    etimerfd *data = (etimerfd *)handle;
    uint64_t count;
    ei_x_buff x;
    ETERM *reply;

    LOGGER_PRINT(__PRETTY_FUNCTION__);

    if(EVENT2FD(event) == data->fd)
    {
        if(read(EVENT2FD(event), &count, sizeof(count)) == sizeof(count))
        {
            driver_select(data->port, FD2EVENT(data->fd),
                          ERL_DRV_READ | ERL_DRV_USE, 0);
            data->ack_pending = true;

            ei_x_new_with_version(&x);
            reply = erl_format("{etimerfd,{timeout,~i}}", count);
            ei_x_encode_term(&x, reply);
            erl_free_compound(reply);
            driver_output(data->port, x.buff, x.index);
        }
        else
        {
            ei_x_new_with_version(&x);
            reply = erl_format("{etimerfd,{~a,~s}}", ATOM_ERROR,
                               "incorrect read size");
            ei_x_encode_term(&x, reply);
            erl_free_compound(reply);
            driver_output(data->port, x.buff, x.index);
        }
    }
}

static void stop_select(ErlDrvEvent event, void *reserved)
{
    LOGGER_PRINT(__PRETTY_FUNCTION__);
}

ErlDrvEntry etimerfd_entry =
{
    init,                       /* init */
    start,                      /* start */
    stop,                       /* stop */
    NULL,                       /* output */
    ready_input,                /* ready_input */
    NULL,                       /* ready_output */
    MODULE,                     /* driver name */
    finish,                     /* finish */
    NULL,                       /* VM reserved */
    control,                    /* control */
    NULL,                       /* timeout */
    NULL,                       /* outputv */
    NULL,                       /* ready_async */
    NULL,                       /* flush */
    NULL,                       /* call */
    NULL,                       /* event */
    ERL_DRV_EXTENDED_MARKER,
    ERL_DRV_EXTENDED_MAJOR_VERSION,
    ERL_DRV_EXTENDED_MINOR_VERSION,
    0,                          /* flags */
    NULL,                       /* VM reserved */
    NULL,                       /* process exit */
    stop_select                 /* stop_select */
};

DRIVER_INIT(etimerfd)
{
    return &etimerfd_entry;
}

