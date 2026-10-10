/*****************************************************************************
  | File        :   Status_Bar.cpp
  | help        :   Top-centre status row (see Status_Bar.h).
******************************************************************************/
#include "Status_Bar.h"

static lv_obj_t *row;

void Status_Bar_Create(void)
{
  row = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(row);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(row, LV_SIZE_CONTENT, 32);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 16, 0);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 54);
}

lv_obj_t *Status_Bar_Row(void)
{
  return row;
}
