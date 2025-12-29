/**
 * @file    fault_handler.c
 * @brief   Fault handler for debugging
 */

#include "fault_handler.h"
#include <stdio.h>
#include <stdbool.h>

static bool fault_dump_enabled = false;

void fault_dump_enable(bool enable)
{
    fault_dump_enabled = enable;
}

bool fault_dump_is_enabled(void)
{
    return fault_dump_enabled;
}
