/*
 * Copyright (C) 2018 bzt (bztsrc@github)
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

// https://github.com/bztsrc/raspi3-tutorial/blob/master/0B_readsector/sd.h (this code is NOT completely mine)

#include "delay.h"

#define MMIO_BASE 0xFE000000

#define SYSTMR_LO ((volatile unsigned int*)(MMIO_BASE+0x00003004))
#define SYSTMR_HI ((volatile unsigned int*)(MMIO_BASE+0x00003008))

void wait_cycles(unsigned int n) {
    if (n) while(n--) {
        asm volatile("nop");
    }
}

void wait_msec(unsigned int n) {
    register unsigned long f, t, r;
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    asm volatile ("mrs %0, cntpct_el0" : "=r"(t));
    
    unsigned long i=((f/1000)*n)/1000;
    do {
        asm volatile ("mrs %0, cntpct_el0" : "=r"(r));
    } while (r-t<i);
}

unsigned long get_sys_timer() {
    unsigned int h=-1, l;
    
    h=*SYSTMR_HI;
    l=*SYSTMR_LO;

    if (h!=*SYSTMR_HI) {
        h=*SYSTMR_HI;
        l=*SYSTMR_LO;
    }

    return ((unsigned long) h << 32) | l;
}

void wait_msec_st(unsigned int n) {
    unsigned long t=get_sys_timer();
    if (t) while(get_sys_timer()-t < n);
}