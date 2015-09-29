/*
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

#include <e_bsp.h>
#include <e-lib.h>
#include "../common.h"

#define BUFFERSIZE 0x3000

void dma_capture(unsigned* resultlist, unsigned count)
{
    volatile unsigned* dmastatusreg = e_get_global_address(e_group_config.core_row, e_group_config.core_col, (void*)E_REG_DMA1STATUS);
    volatile unsigned* dmaconfigreg = e_get_global_address(e_group_config.core_row, e_group_config.core_col, (void*)E_REG_DMA1CONFIG);

    while (count--) {
        *resultlist++ = *dmastatusreg;
        *resultlist++ = *dmaconfigreg;
    }
}

void dma_analyze(unsigned* resultlist, unsigned resultcount)
{
    unsigned prevstatus = -1;
    unsigned prevconfig = -1;
    unsigned similarcount = 0;
    for (int i = 0; i < resultcount; i++)
    {
        unsigned dmastatus = resultlist[2*i];
        unsigned dmaconfig = resultlist[2*i+1];

        if (dmastatus == prevstatus && dmaconfig == prevconfig) {
            similarcount++;
            continue;
        }
        if (similarcount != 0)
            ebsp_message("... for %d iterations", similarcount);

        prevstatus = dmastatus;
        prevconfig = dmaconfig;
        similarcount = 0;

        // Check if DMA is idle, i.e. empty chain
        if ((dmastatus & 0xf) == 0) {
            ebsp_message("DMA idle");
            return;
        }

        void* tasklist[16] = {(void*)-1};
        int count = 0;

        // DMA not idle, so it is working on a descriptor
        e_dma_desc_t* cur = (e_dma_desc_t*)(dmastatus >> 16);

        for(;;) {
            tasklist[count++] = cur;

            // This should not happen
            if (cur == 0)
                break;

            // End of chain: there is no next task
            if ((cur->config & E_DMA_CHAIN) == 0)
                break;

            cur = (e_dma_desc_t*)(cur->config >> 16);
        }

        ebsp_message("CONFIG 0x%x Chain (%d): 0x%x %p %p %p %p %p %p %p",
                dmaconfig,
                count,
                dmastatus,
                tasklist[1],
                tasklist[2],
                tasklist[3],
                tasklist[4],
                tasklist[5],
                tasklist[6],
                tasklist[7]);
    }
}

int main()
{
    bsp_begin();
    int s = bsp_pid();

    char* buffer[2];
    buffer[0] = ebsp_ext_malloc(BUFFERSIZE);
    buffer[1] = ebsp_malloc(BUFFERSIZE);
    for (int i = 0; i < BUFFERSIZE; i++)
    {
        buffer[0][i] = i & 0xff;
        buffer[1][i] = ~(i & 0xff);
    }

    ebsp_dma_handle handle[8];

    if (s == 0) {
        ebsp_message("Handles at %p, %p, %p, %p, %p, %p, %p, %p",
                &handle[0],
                &handle[1],
                &handle[2],
                &handle[3],
                &handle[4],
                &handle[5],
                &handle[6],
                &handle[7]);
    }
    ebsp_barrier();

    //local --> remote
    for (int i = 0; i < 8; i++)
        ebsp_dma_push(&handle[i], buffer[0], buffer[1], BUFFERSIZE);

    if (s == 0)
        ebsp_message("Starting DMA");

    ebsp_barrier();

    unsigned count = BUFFERSIZE / sizeof(int) / 2;
    unsigned *resultlist = (unsigned*)buffer[1];

    ebsp_dma_start(&handle[0]);

    dma_capture(resultlist, count);

    ebsp_barrier();

    if (s == 0) {
        ebsp_message("DMA done");
        dma_analyze(resultlist, count);
    }

    ebsp_barrier();

    EBSP_MSG_ORDERED("%d", s);
    // expect_for_pid: (pid)

    bsp_end();

    return 0;
}
