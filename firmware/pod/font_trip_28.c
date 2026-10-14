/*******************************************************************************
 * Size: 28 px
 * Bpp: 4
 * Opts: --font /Users/sachin/Desktop/learn/bircle/temp/fonts/material/MaterialIconsRound-Regular.otf -r 0xE55D --size 28 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h -o font_trip_28.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef FONT_TRIP_28
#define FONT_TRIP_28 1
#endif

#if FONT_TRIP_28

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+E55D "" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x89, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x5, 0xff, 0x40,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xb, 0xff,
    0xb0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x2f,
    0xff, 0xf2, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x8f, 0xff, 0xf8, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0xef, 0xff, 0xfe, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x5, 0xff, 0xff, 0xff, 0x50, 0x0, 0x0,
    0x0, 0x0, 0xc, 0xff, 0xff, 0xff, 0xc0, 0x0,
    0x0, 0x0, 0x0, 0x2f, 0xff, 0xff, 0xff, 0xf2,
    0x0, 0x0, 0x0, 0x0, 0x9f, 0xff, 0xff, 0xff,
    0xf9, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x0, 0x0, 0x0, 0x6, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x60, 0x0, 0x0, 0xc, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xd0, 0x0, 0x0, 0x3f,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xf3, 0x0, 0x0,
    0xaf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfa, 0x0,
    0x1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x10, 0x7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x70, 0xd, 0xff, 0xff, 0xfa, 0x33, 0xaf,
    0xff, 0xff, 0xd0, 0x3f, 0xff, 0xf8, 0x10, 0x0,
    0x1, 0x8e, 0xff, 0xf3, 0x2e, 0xd6, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x6d, 0xe2, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 448, .box_w = 18, .box_h = 22, .ofs_x = 5, .ofs_y = 3}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 58717, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LV_VERSION_CHECK(8, 0, 0)
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LV_VERSION_CHECK(8, 0, 0)
    .cache = &cache
#endif
};


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LV_VERSION_CHECK(8, 0, 0)
const lv_font_t font_trip_28 = {
#else
lv_font_t font_trip_28 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 22,          /*The maximum line height required by the font*/
    .base_line = -3,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
};



#endif /*#if FONT_TRIP_28*/

