/*******************************************************************************
 * Size: 22 px
 * Bpp: 4
 * Opts: --font /Users/sachin/Desktop/learn/bircle/temp/fonts/material/MaterialIconsRound-Regular.otf -r 0xE405 --size 22 --bpp 4 --format lvgl --no-compress --lv-include lvgl.h -o font_media_22.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef FONT_MEDIA_22
#define FONT_MEDIA_22 1
#endif

#if FONT_MEDIA_22

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+E405 "" */
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x6f, 0xff, 0xc1, 0x0, 0x0, 0x0, 0xef,
    0xff, 0xf6, 0x0, 0x0, 0x0, 0xff, 0xff, 0xf4,
    0x0, 0x0, 0x0, 0xff, 0x99, 0x50, 0x0, 0x0,
    0x0, 0xfd, 0x0, 0x0, 0x0, 0x0, 0x0, 0xfd,
    0x0, 0x0, 0x0, 0x0, 0x0, 0xfd, 0x0, 0x0,
    0x0, 0x0, 0x0, 0xfd, 0x0, 0x0, 0x0, 0x0,
    0x0, 0xfd, 0x0, 0x0, 0x0, 0x5b, 0xc8, 0xfd,
    0x0, 0x0, 0x9, 0xff, 0xff, 0xfd, 0x0, 0x0,
    0x2f, 0xff, 0xff, 0xfd, 0x0, 0x0, 0x6f, 0xff,
    0xff, 0xfd, 0x0, 0x0, 0x5f, 0xff, 0xff, 0xfb,
    0x0, 0x0, 0x1e, 0xff, 0xff, 0xf4, 0x0, 0x0,
    0x4, 0xef, 0xff, 0x80, 0x0, 0x0, 0x0, 0x4,
    0x52, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 352, .box_w = 12, .box_h = 18, .ofs_x = 5, .ofs_y = 2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 58373, .range_length = 1, .glyph_id_start = 1,
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
const lv_font_t font_media_22 = {
#else
lv_font_t font_media_22 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 18,          /*The maximum line height required by the font*/
    .base_line = -2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc           /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
};



#endif /*#if FONT_MEDIA_22*/

