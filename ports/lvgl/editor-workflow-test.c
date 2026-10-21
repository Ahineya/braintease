/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/* Exercise the actual app's commands and modal callbacks with the host adapter. */
#define main editor_application_main
#include "editor.c"
#undef main
#include <assert.h>
#include <sys/stat.h>
static uint32_t workflow_now;
static uint32_t workflow_clock(void) {return workflow_now;}
static void settle_path(void) {
    unsigned rounds=0;
    while(path_dialog&&ui_path_loading(path_dialog)) {
        workflow_now+=2;lv_timer_handler();assert(++rounds<10000);
    }
}
static lv_obj_t *find_button(lv_obj_t *root,const char *caption) {
    unsigned i;
    if(lv_obj_check_type(root,&lv_button_class)) {
        for(i=0;i<lv_obj_get_child_count(root);i++) {
            lv_obj_t *child=lv_obj_get_child(root,i);
            if(lv_obj_check_type(child,&lv_label_class)&&!strcmp(lv_label_get_text(child),caption))return root;
        }
    }
    for(i=0;i<lv_obj_get_child_count(root);i++) {
        lv_obj_t *found=find_button(lv_obj_get_child(root,i),caption);
        if(found)return found;
    }
    return NULL;
}
static void choose(const char *caption) {
    lv_obj_t *button=find_button(lv_layer_top(),caption);
    assert(button);
    lv_obj_send_event(button,LV_EVENT_CLICKED,NULL);
}
static void put_file(const char *path,const char *text) {
    FILE *f=fopen(path,"w");assert(f);assert(fputs(text,f)>=0);assert(!fclose(f));
}
static void equals(const char *path,const char *expected) {
    char text[128];FILE *f=fopen(path,"r");size_t n;assert(f);n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);assert(!strcmp(text,expected));
}
int main(int argc,char **argv) {
    char path[2048],copy[2048];
    ui_document *first,*second;
    unsigned long bounds[2];
    assert(argc==2);
    snprintf(path,sizeof(path),"%s/workflow.txt",argv[1]);
    snprintf(copy,sizeof(copy),"%s/copy.txt",argv[1]);
    documents=ui_documents_create(argv[1]);assert(documents&&ui_documents_new(documents));
    assert(gx_lv_open("Editor workflow test",880,600)==0);
    lv_tick_set_cb(workflow_clock);
    actions=ui_actions_create(commands,sizeof(commands)/sizeof(commands[0]),dispatch,NULL);assert(actions);
    assert(create_ui());
    first=visible;
    assert(!ui_editor_text(editor,"Unicode: \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd1\x96\xd1\x82",21));
    dispatch(CLOSE_TAB,NULL);assert(ui_overlay_active());choose("Cancel");
    assert(!ui_overlay_active()&&ui_documents_count(documents)==1&&visible==first);
    dispatch(SAVE,NULL);assert(path_dialog);settle_path();
    ui_path_set(path_dialog,path);ui_path_submit(path_dialog);
    assert(!ui_overlay_active()&&!text_modified(ui_document_text(first)));
    /* A failed reload preserves both the buffer and its editing position. */
    {
        TextBuffer *original=ui_document_text(first);
        FILE *bad=fopen(path,"wb");assert(bad);assert(fputs("bad",bad)>=0);assert(fputc(0,bad)==0);assert(!fclose(bad));
        ui_editor_select(editor,2,6);
        dispatch(RELOAD,NULL);assert(ui_overlay_active());choose("OK");
        assert(ui_document_text(first)==original&&ui_editor_cursor(editor)==6);
        ui_editor_selection(editor,bounds);assert(bounds[0]==2&&bounds[1]==6);
    }
    dispatch(NEW,NULL);second=visible;assert(second!=first&&ui_documents_count(documents)==2);
    assert(!ui_editor_text(editor,"second",6));
    dispatch(CLOSE_TAB,NULL);choose("Discard");workflow_tick(NULL);
    assert(ui_documents_count(documents)==1&&visible==first);
    /* Save-as cancellation preserves unsaved text and the original path. */
    assert(!ui_editor_text(editor,"!",1));dispatch(SAVE_AS,NULL);assert(path_dialog);
    ui_path_cancel(path_dialog);assert(!ui_overlay_active()&&text_modified(ui_document_text(first)));
    assert(!strcmp(ui_document_path(first),path));
    /* An external edit requires an explicit conflict decision; a copy keeps it. */
    put_file(path,"external");dispatch(SAVE,NULL);workflow_tick(NULL);assert(ui_overlay_active());
    choose("Cancel");equals(path,"external");assert(text_modified(ui_document_text(first)));
    dispatch(SAVE,NULL);workflow_tick(NULL);choose("Save a copy…");workflow_tick(NULL);assert(path_dialog);settle_path();
    ui_path_set(path_dialog,copy);ui_path_submit(path_dialog);
    assert(!ui_overlay_active()&&!text_modified(ui_document_text(first)));
    assert(!strcmp(ui_document_path(first),copy));equals(path,"external");
    /* Native-window close is vetoed while a dirty tab needs a decision. */
    assert(!ui_editor_text(editor,"?",1));assert(close_request()==0);assert(ui_overlay_active());
    choose("Cancel");assert(!quitting&&text_modified(ui_document_text(first)));
    /* Closing a background dirty tab never replaces the active viewport. */
    dispatch(NEW,NULL);second=visible;ui_editor_text(editor,"keep me",7);
    remember_view();ui_documents_select(documents,first);show_document();
    request_close(second);choose("Cancel");assert(visible==first&&ui_documents_count(documents)==2);
    request_close(second);choose("Discard");workflow_tick(NULL);assert(visible==first&&ui_documents_count(documents)==1);
    /* Serial quit cancellation preserves all remaining buffers. */
    dispatch(NEW,NULL);second=visible;ui_editor_text(editor,"dirty",5);
    request_quit();assert(ui_overlay_active());choose("Discard");workflow_tick(NULL);workflow_tick(NULL);
    assert(ui_overlay_active());choose("Cancel");assert(!quitting&&ui_documents_count(documents)==2);
    ui_overlay_close();gx_lv_close();ui_actions_delete(actions);ui_documents_delete(documents);
    puts("EDITOR_WORKFLOW passed tabs, dirty cancel/discard, save/save-as, external-change copy, failed-reload view retention, native close veto and serial quit cancellation");
    return 0;
}
