/* Copyright (C) 2023 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */

/******************************************************************************
 * @file     board.h
 * @brief    BOARD API
 *
 *           copy this file to your project and remove #if 0 / #endif
 ******************************************************************************/
#if 1
#ifndef __BOARD_LIB_H
#define __BOARD_LIB_H

#include <stdint.h>

// <o> Alif Development Kit variant
//     <0=> Alif Development Kit (Generation 1 Silicon Rev A / Board Rev B and Rev C)
//     <1=> Alif AI/ML Application Kit (Generation 1 Silicon Rev A / Board Rev A)
//     <2=> Alif AI/ML Application Kit (Generation 1 Silicon Rev A / Board Rev B)
//     <3=> Alif Development Kit (Generation 2 Silicon Rev B / Internal CoB Board)
//     <4=> Alif Development Kit (Generation 2 Silicon RevBA / Board Rev A, B, C)
//     <5=> Alif AI/ML Application Kit (Generation 2 Silicon Rev B / Board Rev A)
#define BOARD_ALIF_DEVKIT_VARIANT       4

#if (BOARD_ALIF_DEVKIT_VARIANT == 0)
#define BOARD_IS_ALIF_DEVKIT_VARIANT
#include "devkit_gen1/board_defs.h"
#elif (BOARD_ALIF_DEVKIT_VARIANT == 1)
#define BOARD_IS_ALIF_APPKIT_ALPHA1_VARIANT
#include "appkit_alpha1/board_defs.h"
#elif (BOARD_ALIF_DEVKIT_VARIANT == 2)
#define BOARD_IS_ALIF_APPKIT_ALPHA2_VARIANT
#include "appkit_alpha2/board_defs.h"
#elif (BOARD_ALIF_DEVKIT_VARIANT == 3)
#define BOARD_IS_ALIF_DEVKIT_B0_COB_VARIANT
#include "devkit_b0_cob/board_defs.h"
#elif (BOARD_ALIF_DEVKIT_VARIANT == 4)
#define BOARD_IS_ALIF_DEVKIT_B0_VARIANT
#include "devkit_gen2/board_defs.h"
#elif (BOARD_ALIF_DEVKIT_VARIANT == 5)
#define BOARD_IS_ALIF_APPKIT_B1_VARIANT
#include "appkit_gen2/board_defs.h"
#endif

// <o> ILI9806E LCD panel variant
//     <0=> E43RB_FW405
//     <1=> E43GB_MW405
//     <2=> E50RA_MW550
// <i> Defines ILI9806E panel variant
// <i> Default: E43RB_FW405
#define BOARD_ILI9806E_PANEL_VARIANT    0

void BOARD_Pinmux_Init();
void BOARD_Clock_Init();
void BOARD_Power_Init();

typedef void (*BOARD_Callback_t) (uint32_t event);

typedef enum {
	BOARD_BUTTON_ENABLE_INTERRUPT = 1,  /**<BUTTON interrupt enable>*/
	BOARD_BUTTON_DISABLE_INTERRUPT,     /**<BUTTON interrupt disable>*/
} BOARD_BUTTON_CONTROL;

typedef enum {
    BOARD_BUTTON_STATE_LOW,             /**<BUTTON state is LOW>*/
    BOARD_BUTTON_STATE_HIGH,            /**<BUTTON state is HIGH>*/
} BOARD_BUTTON_STATE;

typedef enum {
    BOARD_LED_STATE_LOW,                /**<Drive LED output state LOW>*/
    BOARD_LED_STATE_HIGH,               /**<Drive LED output state HIGH>*/
    BOARD_LED_STATE_TOGGLE,             /**<Toggle LED output state>*/
} BOARD_LED_STATE;

void BOARD_BUTTON1_Init(BOARD_Callback_t user_cb);
void BOARD_BUTTON2_Init(BOARD_Callback_t user_cb);
void BOARD_BUTTON1_Control(BOARD_BUTTON_CONTROL control);
void BOARD_BUTTON2_Control(BOARD_BUTTON_CONTROL control);
void BOARD_BUTTON1_GetState(BOARD_BUTTON_STATE *state);
void BOARD_BUTTON2_GetState(BOARD_BUTTON_STATE *state);
void BOARD_LED1_Control(BOARD_LED_STATE state);
void BOARD_LED2_Control(BOARD_LED_STATE state);

#endif
#endif
