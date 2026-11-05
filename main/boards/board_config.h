#pragma once

#include "sdkconfig.h"

#if CONFIG_TID_BOARD_CYD
#include "cyd_config.h"
#elif CONFIG_TID_BOARD_JC3248W535C
#include "jc3248w535c_config.h"
#else
#error "Select a supported TID display board in menuconfig"
#endif

#define UI_BASE_WIDTH 320
#define UI_BASE_HEIGHT 240

#define UI_SCALE_X(value) \
    (((value) * UI_SCALE_X_NUM + UI_SCALE_X_DEN / 2) / UI_SCALE_X_DEN)
#define UI_SCALE_Y(value) \
    (((value) * UI_SCALE_Y_NUM + UI_SCALE_Y_DEN / 2) / UI_SCALE_Y_DEN)
