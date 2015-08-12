/*
File: bsp_host.c

This file is part of the Epiphany BSP library.

Copyright (C) 2014 Buurlage Wits
Support e-mail: <info@buurlagewits.nl>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License (LGPL)
as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
and the GNU Lesser General Public License along with this program,
see the files COPYING and COPYING.LESSER. If not, see
<http://www.gnu.org/licenses/>.
*/

#include "host_bsp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <e-loader.h>
#include "common.h"

#define __USE_XOPEN2K
#include <unistd.h>  // readlink, for getting the path to the executable
#define __USE_POSIX199309 1
#include <time.h>

// Global BSP state
typedef struct
{
    // The number of processors available
    int nprocs;

    // The directory of the program and e-program
    // including a trailing slash
    char e_directory[1024];
    // The name of the e-program
    char e_fullpath[1024];

    // Number of rows or columns in use
    int rows;
    int cols;

    // Number of processors in use
    int nprocs_used;

    // External memory that holds ebsp_comm_buf
    e_mem_t emem;
    // External memory that holds the mallocs
    e_mem_t emem_malloc;

    // Local copy of ebsp_comm_buf to copy from and
    // copy into.
    ebsp_comm_buf comm_buf;
    // For reading out the final queue
    int message_index;

    void (*sync_callback)(void);
    void (*end_callback)(void);

    int num_vars_registered;

    // Epiphany specific variables
    e_platform_t platform;
    e_epiphany_t dev;

    // Timer storage
    struct timespec ts_start, ts_end;
} bsp_state_t;

bsp_state_t state;
int bsp_initialized = 0;;

void _host_sync();
void _microsleep(int microseconds);  // 1000 gives 1 millisecond
void _get_p_coords(int pid, int* row, int* col);
void init_application_path();

void ebsp_malloc_init(void* external_memory_base);
void* ebsp_ext_malloc(unsigned int nbytes);
void ebsp_free(void* ptr);

int ebsp_write(int pid, void* src, off_t dst, int size)
{
    int prow, pcol;
    _get_p_coords(pid, &prow, &pcol);
    if (e_write(&state.dev,
                prow, pcol,
                dst, src, size) != size)
    {
        fprintf(stderr,
                "ERROR: e_write(dev,%d,%d,%p,%p,%d) failed in ebsp_write.\n",
                prow, pcol, (void*)dst, (void*)src, size);
        return 0;
    }
    return 1;
}

int ebsp_read(int pid, off_t src, void* dst, int size)
{
    int prow, pcol;
    _get_p_coords(pid, &prow, &pcol);
    if (e_read(&state.dev,
                prow, pcol,
                src, dst, size) != size)
    {
        fprintf(stderr,
                "ERROR: e_read(dev,%d,%d,%p,%p,%d) failed in ebsp_read.\n",
                prow, pcol, (void*)src, (void*)dst, size);
        return 0;
    }
    return 1;
}

int _write_core_syncstate(int pid, int syncstate)
{
    return ebsp_write(pid, &syncstate, (off_t)state.comm_buf.syncstate_ptr, 4);
}

int _write_extmem(void* src, off_t offset, int size)
{
    if (e_write(&state.emem, 0, 0, offset, src, size) != size)
    {
        fprintf(stderr, "ERROR: _write_extmem(src,%p,%d) failed.\n",
                (void*)offset, size);
        return 0;
    }
    return 1;
}

int bsp_init(const char* _e_name,
        int argc,
        char **argv)
{
    if (bsp_initialized) {
        fprintf(stderr, "ERROR: bsp_init called when already initialized.\n");
        return 0;
    }

    // Get the path to the application and append the epiphany executable name
    init_application_path();
    snprintf(state.e_fullpath, sizeof(state.e_fullpath), "%s%s",
            state.e_directory, _e_name);

    // Check if the file exists
    if (access(state.e_fullpath, R_OK) == -1)
    {
        fprintf(stderr, "ERROR: Could not find epiphany executable: %s\n",
                state.e_fullpath);
        return 0;
    }

    // Initialize the Epiphany system for the working with the host application
    if (e_init(NULL) != E_OK) {
        fprintf(stderr, "ERROR: Could not initialize HAL data structures.\n");
        return 0;
    }

    // Reset the Epiphany system
    if (e_reset_system() != E_OK) {
        fprintf(stderr, "ERROR: Could not reset the Epiphany system.\n");
        return 0;
    }

    // Get information on the platform
    if (e_get_platform_info(&state.platform) != E_OK) {
        fprintf(stderr, "ERROR: Could not obtain platform information.\n");
        return 0;
    }

    // Obtain the number of processors from the platform information
    state.nprocs = state.platform.rows * state.platform.cols;

    bsp_initialized = 1;

    return 1;
}

int bsp_begin(int nprocs)
{
    // TODO(*)
    // When one of the functions fails half-way in bsp_begin
    // Then the functions that DID succeed should be undone again
    // So at e_load_group failure it cleanup the e_open result

    if (nprocs < 1 || nprocs > _NPROCS) {
        fprintf(stderr, "ERROR: nprocs = %d.\n", nprocs);
        return 0;
    }

    // TODO(*) non-rectangle
    state.rows = (nprocs / state.platform.rows);
    state.cols = nprocs / (nprocs / state.platform.rows);

#ifdef DEBUG
    printf("(BSP) INFO: Making a workgroup of size %i x %i\n",
            state.rows,
            state.cols);
#endif

    state.nprocs_used = nprocs;
    state.num_vars_registered = 0;

    // Open the workgroup
    if (e_open(&state.dev,
                0, 0,
                state.rows,
                state.cols) != E_OK) {
        fprintf(stderr, "ERROR: Could not open workgroup.\n");
        return 0;
    }

    if (e_reset_group(&state.dev) != E_OK) {
        fprintf(stderr, "ERROR: Could not reset workgroup.\n");
        return 0;
    }

    // Load the e-binary
#ifdef DEBUG
    printf("(BSP) INFO: Loading: %s\n", state.e_fullpath);
#endif
    if (e_load_group(state.e_fullpath,
                &state.dev,
                0, 0,
                state.rows, state.cols,
                E_FALSE) != E_OK) {
        fprintf(stderr, "ERROR: Could not load program in workgroup.\n");
        return 0;
    }

    // Allocate communication buffer
    if (e_alloc(&state.emem, COMMBUF_OFFSET, sizeof(ebsp_comm_buf)) != E_OK) {
        fprintf(stderr, "ERROR: e_alloc for comm_buf failed in bspbegin.\n");
        return 0;
    }

    // Allocate buffer for mallocs
    if (e_alloc(&state.emem_malloc, DYNMEM_OFFSET, DYNMEM_SIZE) != E_OK) {
        fprintf(stderr, "ERROR: e_alloc for malloc failed in bspbegin.\n");
        return 0;
    }

    ebsp_malloc_init(state.emem_malloc.base);

    // Set initial buffer to zero so that it can be filled by messages
    // before calling ebsp_spmd
    memset(&state.comm_buf, 0, sizeof(ebsp_comm_buf));

    return 1;
}

void ebsp_set_sync_callback(void (*cb)())
{
    state.sync_callback = cb;
}

void ebsp_set_end_callback(void (*cb)())
{
    state.end_callback = cb;
}

void _update_remote_timer()
{
    // Current time. Repeat these lines every iteration
    clock_gettime(CLOCK_MONOTONIC, &state.ts_end);

    float time_elapsed = (state.ts_end.tv_sec - state.ts_start.tv_sec +
            (state.ts_end.tv_nsec - state.ts_start.tv_nsec) * 1.0e-9);

    _write_extmem(&time_elapsed,
            offsetof(ebsp_comm_buf, remotetimer),
            sizeof(float));
}

int ebsp_spmd()
{
    // Write communication buffer containing nprocs,
    // messages and tagsize
    state.comm_buf.nprocs = state.nprocs_used;
    for (int i = 0; i < state.nprocs; ++i)
        state.comm_buf.syncstate[i] = STATE_INIT;
    if (!_write_extmem(&state.comm_buf, 0, sizeof(ebsp_comm_buf)))
    {
        fprintf(stderr, "ERROR: initial extmem write failed in ebsp_spmd.\n");
        return 0;
    }

    // Starting time
    clock_gettime(CLOCK_MONOTONIC, &state.ts_start);
    _update_remote_timer();

    // Start the program
    // Only in DEBUG mode:
    // The program will block on bsp_begin in state STATE_EREADY
    // untill we send a STATE_CONTINUE
    if (e_start_group(&state.dev) != E_OK) {
        fprintf(stderr, "ERROR: e_start_group() failed.\n");
        return 0;
    }

    // Every iteration we only have to read the start of the buffer
    // because that is where the syncstate flags are located
    // We have to read everything up to remotetimer (not included)
    const int read_size = offsetof(ebsp_comm_buf, remotetimer);

#ifdef DEBUG
    int cores_initialized;
    while (1)
    {
        _microsleep(1000);  // 1 millisecond

        // Read the communication buffer
        if (e_read(&state.emem, 0, 0, 0, &state.comm_buf, read_size)
                != read_size) {
            fprintf(stderr,
                    "ERROR: e_read ebsp_comm_buf failed in ebsp_spmd.\n");
            return 0;
        }

        // Check every core
        cores_initialized = 0;
        for (int i = 0; i < state.nprocs; ++i)
            if (state.comm_buf.syncstate[i] == STATE_EREADY)
                ++cores_initialized;
        if (cores_initialized == state.nprocs)
            break;
    }
    printf("(BSP) DEBUG: All epiphany cores are ready for initialization.\n");
    printf("(BSP) DEBUG: ebsp uses %d KB = %p B of external memory.\n",
            sizeof(ebsp_comm_buf)/1024, (void*)sizeof(ebsp_comm_buf));

    _update_remote_timer();

    // Send start signal
    for (int i = 0; i < state.nprocs; ++i)
        _write_core_syncstate(i, STATE_CONTINUE);
#endif

    int total_syncs = 0;
    int extmem_corrupted = 0;
    int run_counter;
    int sync_counter;
    int finish_counter;
    int continue_counter;
    int abort_counter;

#ifdef DEBUG
    int iter = 0;
    printf("(BSP) DEBUG: All epiphany cores initialized.\n");
#endif

    for (;;)
    {
        _update_remote_timer();
        _microsleep(1);  // 1000 is 1 millisecond

        // Read the first part of the communication buffer
        // that contains sync states: read all up till coredata (not inclusive)
        if (e_read(&state.emem, 0, 0, 0, &state.comm_buf, read_size)
                != read_size) {
            fprintf(stderr,
                    "ERROR: e_read ebsp_comm_buf failed in ebsp_spmd.\n");
            return 0;
        }

        // Check sync states
        run_counter      = 0;
        sync_counter     = 0;
        finish_counter   = 0;
        continue_counter = 0;
        abort_counter    = 0;
        for (int i = 0; i < state.nprocs; i++) {
            switch (state.comm_buf.syncstate[i]){
                case STATE_INIT:
                    break;

                case STATE_RUN:
                    run_counter++;
                    break;

                case STATE_SYNC:
                    sync_counter++;
                    break;

                case STATE_FINISH:
                    finish_counter++;
                    break;

                case STATE_CONTINUE:
                    continue_counter++;
                    break;

                case STATE_ABORT:
                    abort_counter++;
                    break;

                default:
                    extmem_corrupted++;
                    if (extmem_corrupted <= 32)  // to avoid overflow
                        fprintf(stderr,
                                "ERROR: External memory corrupted."
                                " syncstate[%d] = %d.\n",
                                i, state.comm_buf.syncstate[i]);
                    break;
            }
        }

        // Check messages
        if (state.comm_buf.msgflag)
        {
            printf("$%02d: %s\n",
                    state.comm_buf.msgflag - 1,  // flag = pid+1
                    state.comm_buf.msgbuf);
            // Reset flag to let epiphany cores continue
            state.comm_buf.msgflag = 0;
            // Write the int to the external comm_buf
            _write_extmem((void*)&state.comm_buf.msgflag,
                    offsetof(ebsp_comm_buf, msgflag),
                    sizeof(int));
        }

#ifdef DEBUG
        if (iter % 1000 == 0) {
            printf("Current time: %E seconds\n", time_elapsed);
            printf("run %02d - sync %02d - finish %02d - continue %02d\n",
                    run_counter, sync_counter, finish_counter,
                    continue_counter);
        }
        ++iter;
#endif

        if (sync_counter == state.nprocs) {
            ++total_syncs;
#ifdef DEBUG
            // This part of the sync (host side)
            // usually does not crash so only one
            // line of debug output is needed here
            printf("(BSP) DEBUG: Sync %d after %f seconds\n",
                    total_syncs, time_elapsed);
#endif
            // if call back, call and wait
            if (state.sync_callback)
                state.sync_callback();

            // First reset the comm_buf
            for (int i = 0; i < state.nprocs; i++)
                state.comm_buf.syncstate[i] = STATE_CONTINUE;
            _write_extmem(&state.comm_buf.syncstate,
                    offsetof(ebsp_comm_buf, syncstate),
                    _NPROCS * sizeof(int));
            // Now write it to all cores to continue their execution
            for (int i = 0; i < state.nprocs; i++)
                _write_core_syncstate(i, STATE_CONTINUE);
        }
        if (abort_counter != 0) {
            printf("(BSP) ERROR: bsp_abort was called\n");
            break;
        }
        if (finish_counter == state.nprocs)
            break;
    }
    // Read the communication buffer
    // to get final messages from the program
    if (e_read(&state.emem, 0, 0, 0, &state.comm_buf, sizeof(ebsp_comm_buf))
            != sizeof(ebsp_comm_buf))
    {
        fprintf(stderr,
                "ERROR: e_read full ebsp_comm_buf failed in ebsp_spmd.\n");
        return 0;
    }

#ifdef DEBUG
    printf("(BSP) INFO: Program finished\n");
#endif

    if (state.end_callback)
        state.end_callback();

    return 1;
}

int bsp_end()
{
    if (!bsp_initialized) {
        fprintf(stderr,
                "ERROR: bsp_end called when bsp was not initialized.\n");
        return 0;
    }

    e_free(&state.emem);

    if (E_OK != e_finalize()) {
        fprintf(stderr, "ERROR: Could not finalize the Epiphany connection.\n");
        return 0;
    }

    memset(&state, 0, sizeof(state));
    bsp_initialized = 0;

    if (state.end_callback)
        state.end_callback();

    return 1;
}

int bsp_nprocs()
{
    return state.nprocs;
}

void ebsp_set_tagsize(int *tag_bytes)
{
    int oldsize = state.comm_buf.tagsize;
    state.comm_buf.tagsize = *tag_bytes;
    *tag_bytes = oldsize;
}

// Converting between epiphany and arm pointers
// Only for things stored in state.comm_buf !

void* _arm_to_e_pointer(void* ptr)
{
    return (void*)((unsigned int)ptr
            - (unsigned int)&state.comm_buf
            + COMMBUF_EADDR);
}

void* _e_to_arm_pointer(void* ptr)
{
    return (void*)((unsigned int)ptr
            - COMMBUF_EADDR
            + (unsigned int)&state.comm_buf);
}

void ebsp_send_down(int pid, const void *tag, const void *payload, int nbytes)
{
    ebsp_message_queue* q = &state.comm_buf.message_queue[0];
    unsigned int index = q->count;
    unsigned int payload_offset = state.comm_buf.data_payloads.buffer_size;
    unsigned int total_nbytes = state.comm_buf.tagsize + nbytes;
    void *tag_ptr;
    void *payload_ptr;

    if (index >= MAX_MESSAGES) {
        fprintf(stderr,
                "ERROR: Maximal message count reached in ebsp_send_down.\n");
        return;
    }
    if (payload_offset + total_nbytes > MAX_PAYLOAD_SIZE) {
        fprintf(stderr,
                "ERROR: Maximal data payload sent in ebsp_send_down.\n");
        return;
    }

    q->count++;
    state.comm_buf.data_payloads.buffer_size += total_nbytes;

    tag_ptr = &state.comm_buf.data_payloads.buf[payload_offset];
    payload_offset += state.comm_buf.tagsize;
    payload_ptr = &state.comm_buf.data_payloads.buf[payload_offset];

    q->message[index].pid = pid;
    q->message[index].tag = _arm_to_e_pointer(tag_ptr);
    q->message[index].payload = _arm_to_e_pointer(payload_ptr);
    q->message[index].nbytes = nbytes;
    memcpy(tag_ptr, tag, state.comm_buf.tagsize);
    memcpy(payload_ptr, payload, nbytes);
}

int ebsp_get_tagsize()
{
    return state.comm_buf.tagsize;
}

void ebsp_qsize(int *packets, int *accum_bytes)
{
    *packets = 0;
    *accum_bytes = 0;

    ebsp_message_queue* q = &state.comm_buf.message_queue[0];
    int mindex = state.message_index;
    int qsize = q->count;

    // Count everything after mindex
    for (; mindex < qsize; mindex++)
    {
        *packets += 1;
        *accum_bytes += q->message[mindex].nbytes;
    }
    return;
}

ebsp_message_header* _next_queue_message()
{
    ebsp_message_queue* q = &state.comm_buf.message_queue[0];
    if (state.message_index < q->count)
        return &q->message[state.message_index];
    return 0;
}

void _pop_queue_message()
{
    state.message_index++;
}

void ebsp_get_tag(int *status, void *tag)
{
    ebsp_message_header* m = _next_queue_message();
    if (m == 0)
    {
        *status = -1;
        return;
    }
    *status = m->nbytes;
    memcpy(tag, _e_to_arm_pointer(m->tag), state.comm_buf.tagsize);
}

void ebsp_move(void *payload, int buffer_size)
{
    ebsp_message_header* m = _next_queue_message();
    _pop_queue_message();
    if (m == 0)
    {
        // This part is not defined by the BSP standard
        return;
    }

    if (buffer_size == 0)  // Specified by BSP standard
        return;

    if (m->nbytes < buffer_size)
        buffer_size = m->nbytes;

    memcpy(payload, _e_to_arm_pointer(m->payload), buffer_size);
}

int ebsp_hpmove(void **tag_ptr_buf, void **payload_ptr_buf)
{
    ebsp_message_header* m = _next_queue_message();
    _pop_queue_message();
    if (m == 0) return -1;
    *tag_ptr_buf = _e_to_arm_pointer(m->tag);
    *payload_ptr_buf = _e_to_arm_pointer(m->payload);
    return m->nbytes;
}

void ebsp_send_buffered(void* src, int dst_core_id, int nbytes)
{

    void* exmem_in_buffer = ebsp_ext_malloc(nbytes);
    if (exmem_in_buffer == 0)
    {
        printf("ERROR: not enough memory in exmem for ebsp_send_buffered");
        return;
    }
    memcpy(src, exmem_in_buffer, nbytes);
    ebsp_comm_buf.exmem_next_in_chunk[dst_core_id] = exmem_in_buffer;
}

void ebsp_get_buffered(int dst_core_id, int max_nbytes)
{
    void* exmem_out_buffer = ebsp_ext_malloc(max_nbytes);
    if (exmem_out_buffer == 0)
    {
        printf("ERROR: not enough memory in exmem for ebsp_send_buffered");
        return;
    }
    ebsp_comm_buf.exmem_current_out_chunk[dst_core_id] = exmem_out_buffer;
}

//------------------
// Private functions
//------------------

void _microsleep(int microseconds)
{
    struct timespec request, remain;
    request.tv_sec = (int)(microseconds / 1000000);
    request.tv_nsec = (microseconds - 1000000 * request.tv_sec) * 1000;
    if (clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &remain) != 0)
        fprintf(stderr, "ERROR: clock_nanosleep was interrupted.\n");
}

void _get_p_coords(int pid, int* row, int* col)
{
    (*row) = pid / state.cols;
    (*col) = pid % state.cols;
}

// Get the directory that the application is running in
// and store it in state.e_directory
// It will include a trailing slash
void init_application_path()
{
    char path[1024];
    if (readlink("/proc/self/exe", path, 1024) > 0) {
        char * slash = strrchr(path, '/');
        if (slash)
        {
            int count = slash-path + 1;
            memcpy(state.e_directory, path, count);
            state.e_directory[count+1] = 0;
        }
    } else {
        fprintf(stderr, "ERROR: Could not find process directory.\n");
        memcpy(state.e_directory, "./", 3);  // including terminating 0
    }
    return;
}
