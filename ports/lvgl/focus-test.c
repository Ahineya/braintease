/* SPDX-License-Identifier: MIT */
#include "ui.h"
#include "ui-data.h"
#include "ui-editor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void gx_lv_host_active(int active);
static unsigned focus_events,blur_events,click_events,flushes;
static unsigned width,height;
static unsigned char *frame;
static lv_display_flush_cb_t original_flush;
static void capture(lv_display_t *display,const lv_area_t *area,uint8_t *map) {
    uint32_t stride=lv_display_get_buf_active(display)->header.stride;
    int32_t x,y;
    for(y=area->y1;y<=area->y2;y++)for(x=area->x1;x<=area->x2;x++) {
        const uint8_t *source=map+(y-area->y1)*stride+(x-area->x1)*3;
        unsigned char *pixel=frame+((unsigned long)y*width+x)*3;
        pixel[0]=source[2];pixel[1]=source[1];pixel[2]=source[0];
    }
    flushes++;original_flush(display,area,map);
}
static uint32_t pixel(int32_t x,int32_t y) {
    unsigned char *p=frame+((unsigned long)y*width+x)*3;
    assert(x>=0&&y>=0&&(unsigned)x<width&&(unsigned)y<height);
    return (uint32_t)p[0]<<16|(uint32_t)p[1]<<8|p[2];
}
static void observed(lv_event_t *event) {
    lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_FOCUSED)focus_events++;
    if(code==LV_EVENT_DEFOCUSED)blur_events++;
    if(code==LV_EVENT_CLICKED)click_events++;
}
static void selected(lv_obj_t *input) {
    lv_obj_t *label=lv_textarea_get_label(input);
    assert(lv_label_get_text_selection_start(label)==2&&lv_label_get_text_selection_end(label)==7);
    assert(lv_textarea_get_cursor_pos(input)==5);
}
static void input_checks(void) {
    lv_group_t *group=gx_lv_group(),*temporary;
    lv_obj_t *input=ui_input(lv_screen_active(),"Text"),*other,*parent,*child;
    lv_state_t saved;
    unsigned before;
    gx_lv_pos(input,16,16);gx_lv_size(input,360,30);
    lv_textarea_set_text(input,"selection");lv_group_focus_obj(input);
    lv_group_set_editing(group,true);lv_obj_add_state(input,LV_STATE_FOCUS_KEY);
    lv_textarea_set_cursor_pos(input,5);
    lv_label_set_text_selection_start(lv_textarea_get_label(input),2);
    lv_label_set_text_selection_end(lv_textarea_get_label(input),7);
    lv_obj_add_event_cb(input,observed,LV_EVENT_ALL,NULL);
    saved=lv_obj_get_state(input);
    gx_lv_host_active(0);ui_focus_suspend(group);
    assert(lv_group_get_focused(group)==input&&lv_group_get_editing(group));
    assert(!(lv_obj_get_state(input)&(LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY|LV_STATE_EDITED)));
    assert(lv_obj_get_style_border_width(input,LV_PART_CURSOR)==0);
    assert(lv_color_eq(lv_obj_get_style_bg_color(input,LV_PART_SELECTED),lv_color_hex(0x30343c)));
    selected(input);assert(!focus_events&&!blur_events&&!click_events);
    lv_refr_now(lv_display_get_default());before=flushes;
    ui_focus_suspend(group);lv_refr_now(lv_display_get_default());assert(flushes==before);
    gx_lv_host_active(1);ui_focus_resume(group);
    assert(lv_obj_get_state(input)==saved&&lv_group_get_editing(group));selected(input);
    assert(lv_obj_get_style_border_width(input,LV_PART_CURSOR)==gx_lv_px(1));
    assert(!focus_events&&!blur_events&&!click_events);
    /* Frozen traversal still suspends and restores without being unfrozen. */
    other=ui_button(lv_screen_active(),"Other",UI_BUTTON);gx_lv_pos(other,16,56);
    lv_group_focus_freeze(group,true);gx_lv_host_active(0);ui_focus_suspend(group);
    lv_group_focus_obj(other);assert(lv_group_get_focused(group)==input);
    gx_lv_host_active(1);ui_focus_resume(group);assert(lv_group_get_focused(group)==input);
    lv_group_focus_obj(other);assert(lv_group_get_focused(group)==input);
    lv_group_focus_freeze(group,false);
    /* A focus change made while inactive is captured by the next suspension. */
    gx_lv_host_active(0);ui_focus_suspend(group);lv_group_focus_obj(other);ui_focus_suspend(group);
    assert(!lv_obj_has_state(other,LV_STATE_FOCUSED));
    gx_lv_host_active(1);ui_focus_resume(group);assert(lv_obj_has_state(other,LV_STATE_FOCUSED));
    /* Matching deletion callbacks prevent restoring a dead focused control. */
    lv_group_focus_obj(input);gx_lv_host_active(0);ui_focus_suspend(group);lv_obj_delete(input);
    ui_focus_suspend(group);gx_lv_host_active(1);ui_focus_resume(group);
    assert(lv_group_get_focused(group)==other&&lv_obj_has_state(other,LV_STATE_FOCUSED));
    /* An already-deleted modal group must never be dereferenced on restore. */
    ui_message("Modal","Background completion");temporary=gx_lv_group();
    assert(temporary!=group);gx_lv_host_active(0);ui_focus_suspend(temporary);
    ui_overlay_close();assert(gx_lv_group()==group);ui_focus_suspend(group);
    gx_lv_host_active(1);ui_focus_resume(group);assert(lv_group_get_focused(group)==other);
    /* Group deletion may leave its objects alive and detached. */
    temporary=lv_group_create();gx_lv_set_group(temporary);child=ui_input(lv_screen_active(),"Detached");
    lv_group_focus_obj(child);gx_lv_host_active(0);ui_focus_suspend(temporary);
    lv_group_delete(temporary);gx_lv_set_group(group);ui_focus_suspend(group);
    gx_lv_host_active(1);ui_focus_resume(group);assert(!lv_obj_get_group(child));lv_obj_delete(child);
    /* Hiding an ancestor while suspended cannot revive an inaccessible field. */
    parent=ui_stack(lv_screen_active(),LV_FLEX_FLOW_COLUMN,0);child=ui_input(parent,"Hidden");
    lv_group_focus_obj(child);gx_lv_host_active(0);ui_focus_suspend(group);lv_obj_set_hidden(parent,true);
    gx_lv_host_active(1);ui_focus_resume(group);assert(!lv_group_get_focused(group));lv_obj_delete(parent);
    /* Disabled targets are treated the same way, including a frozen group. */
    lv_group_focus_obj(other);gx_lv_host_active(0);ui_focus_suspend(group);
    lv_group_focus_freeze(group,true);lv_obj_add_state(other,LV_STATE_DISABLED);
    gx_lv_host_active(1);ui_focus_resume(group);assert(!lv_group_get_focused(group));
    lv_group_focus_freeze(group,false);lv_obj_delete(other);
}
static void editor_checks(void) {
    TextBuffer *buffer=text_new();ui_editor *editor;
    lv_area_t content;
    int32_t x,y,h;
    unsigned long selection[2],revision;
    assert(buffer&&!text_append(buffer,(const unsigned char *)"\n\n",2));text_saved(buffer);
    editor=ui_editor_create(lv_screen_active(),buffer);assert(editor);
    gx_lv_pos(ui_editor_object(editor),16,100);gx_lv_size(ui_editor_object(editor),360,180);
    ui_editor_focus(editor);lv_obj_update_layout(lv_screen_active());
    assert(ui_editor_caret(editor,&x,&y,&h));lv_obj_get_content_coords(ui_editor_object(editor),&content);
    lv_refr_now(lv_display_get_default());assert(pixel(content.x1+x,content.y1+y+h/2)==0x9ab8ef);
    revision=text_revision(buffer);gx_lv_host_active(0);ui_focus_suspend(gx_lv_group());
    assert(!ui_editor_focused(editor));lv_refr_now(lv_display_get_default());
    assert(pixel(content.x1+x,content.y1+y+h/2)==0x212225);
    assert(ui_editor_cursor(editor)==0&&text_revision(buffer)==revision&&!text_modified(buffer));
    gx_lv_host_active(1);ui_focus_resume(gx_lv_group());lv_refr_now(lv_display_get_default());
    assert(ui_editor_focused(editor)&&pixel(content.x1+x,content.y1+y+h/2)==0x9ab8ef);
    ui_editor_select(editor,0,1);gx_lv_host_active(0);ui_focus_suspend(gx_lv_group());
    ui_editor_selection(editor,selection);assert(selection[0]==0&&selection[1]==1);
    gx_lv_host_active(1);ui_focus_resume(gx_lv_group());ui_editor_selection(editor,selection);
    assert(selection[0]==0&&selection[1]==1&&!text_modified(buffer));
    lv_obj_delete(ui_editor_object(editor));text_free(buffer);
}
int main(void) {
    lv_display_t *display;
    assert(!gx_lv_open("Window focus checks",480,320));ui_screen();
    display=lv_display_get_default();width=480*gx_lv_density();height=320*gx_lv_density();
    frame=calloc((unsigned long)width*height*3,1);assert(frame);
    original_flush=lv_display_get_flush_cb(display);lv_display_set_flush_cb(display,capture);
    input_checks();editor_checks();gx_lv_close();free(frame);
    puts("FOCUS passed retained input selection/editing, exact visual restoration, idle, frozen groups, deletion/modal lifetime, hidden/disabled controls and editor caret pixels");
    return 0;
}
