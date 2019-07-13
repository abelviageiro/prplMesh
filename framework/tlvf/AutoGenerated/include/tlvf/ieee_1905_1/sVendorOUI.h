///////////////////////////////////////
// AUTO GENERATED FILE - DO NOT EDIT //
///////////////////////////////////////
/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Copyright (c) 2016-2019 Intel Corporation
 *
 * This code is subject to the terms of the BSD+Patent license.
 * See LICENSE file for more details.
 */

#ifndef _SUINT_24_t_H_
#define _SUINT_24_t_H_

#include <stdint.h>

typedef struct sVendorOUI {
    uint8_t upper;
    uint8_t middle;
    uint8_t lower;
    sVendorOUI(uint32_t val) 
    {
        lower = val & 0xFF;
        middle = (val >> 8 ) & 0xFF;
        upper = (val >> 16 ) & 0xFF;
    }

    operator uint32_t() const
    {
        return (upper << 16) + (middle << 8) + lower;
    }

    void struct_swap()
    {
        //Using xor variable swapping in order to avoid allocating another variable
        upper ^=lower;
        lower ^=upper;
        upper ^=lower;
    }

    void struct_init() { }
} __attribute__((packed)) sOUI;


#endif //_SUINT_24_t_H_
