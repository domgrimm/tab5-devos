/**
 * @file lv_conf.h
 * Configuration file for devOS LVGL v9
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#ifndef __ASSEMBLY__
#include <stdint.h>
#endif

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 32

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#define LV_LIMITS_INCLUDE       <limits.h>
#define LV_STDARG_INCLUDE       <stdarg.h>

#define LV_USE_DRAW_SW_ASM      LV_DRAW_SW_ASM_NONE

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DEF_REFR_PERIOD      16      /* [ms] ~60 FPS */
#define LV_DPI_DEF              294     /* [px/inch] Tab5 5.0" 1280x720 */

/*=================
 * OPERATING SYSTEM
 *=================*/
#ifdef ESP_PLATFORM
    #define LV_USE_OS           LV_OS_FREERTOS
#else
    #define LV_USE_OS           LV_OS_PTHREAD
#endif

/*====================
   RENDERING & DRAW
 *====================*/
#define LV_USE_DRAW_SW          1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1

#ifdef ESP_PLATFORM
    #define LV_USE_PPA          1
#endif

/*==================
 * FONT USAGE
 *==================*/
#define LV_FONT_MONTSERRAT_8    0
#define LV_FONT_MONTSERRAT_10   1
#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_18   1
#define LV_FONT_MONTSERRAT_20   1
#define LV_FONT_MONTSERRAT_22   1
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_28   1
#define LV_FONT_UNSCII_8        1

#define LV_FONT_DEFAULT         &lv_font_montserrat_14

/* Enable font subpixel positioning */
#define LV_USE_FONT_SUBPX       0

/*==================
 * TEXT SETTINGS
 *==================*/
#define LV_TXT_ENC              LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS      " ,.;:-_)]}"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_COLOR_CMD        "#"

/*==================
 * WIDGETS
 *==================*/
#define LV_USE_ANIMIMG          1
#define LV_USE_ARC              1
#define LV_USE_BAR              1
#define LV_USE_BUTTON           1
#define LV_USE_BUTTONMATRIX     1
#define LV_USE_CALENDAR         1
#define LV_USE_CANVAS           1
#define LV_USE_CHECKBOX         1
#define LV_USE_DROPDOWN         1
#define LV_USE_IMAGE            1
#define LV_USE_IMAGEBUTTON      1
#define LV_USE_KEYBOARD         1
#define LV_USE_LABEL            1
#define LV_LABEL_TEXT_SELECTION 1
#define LV_LABEL_LONG_TXT_HINT  1
#define LV_USE_LED              1
#define LV_USE_LINE             1
#define LV_USE_LIST             1
#define LV_USE_MENU             1
#define LV_USE_MSGBOX           1
#define LV_USE_ROLLER           1
#define LV_USE_SCALE            1
#define LV_USE_SLIDER           1
#define LV_USE_SPAN             1
#define LV_USE_SPINBOX          1
#define LV_USE_SPINNER          1
#define LV_USE_SWITCH           1
#define LV_USE_TEXTAREA         1
#define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#define LV_USE_TABLE            1
#define LV_USE_TABVIEW          1
#define LV_USE_TILEVIEW         1
#define LV_USE_WIN              1

/*==================
 * THEMES
 *==================*/
#define LV_USE_THEME_DEFAULT    1
#define LV_THEME_DEFAULT_DARK   1
#define LV_THEME_DEFAULT_GROW   1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80

/*==================
 * LAYOUTS
 *==================*/
#define LV_USE_FLEX             1
#define LV_USE_GRID             1

/*==================
 * DRIVERS
 *==================*/
#ifndef ESP_PLATFORM
    #define LV_USE_SDL          1
    #if LV_USE_SDL
        #define LV_SDL_INCLUDE_PATH     <SDL2/SDL.h>
        #define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
        #define LV_SDL_BUF_COUNT        1
        #define LV_SDL_ACCELERATED      1
        #define LV_SDL_FULLSCREEN       0
        #define LV_SDL_DIRECT_EXIT      1
        #define LV_SDL_MOUSEWHEEL_MODE  LV_SDL_MOUSEWHEEL_MODE_ENCODER
    #endif
#endif

#endif /* LV_CONF_H */
