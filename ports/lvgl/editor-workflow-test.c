/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
/* Exercise the actual app's commands and modal callbacks with the host adapter. */
#define main editor_application_main
#include "editor.c"
#undef main
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
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
static void editor_layout(void) {
    lv_obj_t *screen=lv_screen_active(),*strip=ui_tabs_object(tabs),*text=ui_editor_object(editor);
    lv_area_t strip_area,text_area;
    lv_obj_update_layout(screen);
    assert(lv_obj_get_child_count(screen)==3);
    assert(lv_obj_get_child(screen,0)==menubar&&lv_obj_get_child(screen,1)==document_area&&lv_obj_get_child(screen,2)==status);
    assert(lv_obj_get_child_count(menubar)==4&&lv_obj_get_child_count(document_area)==2);
    assert(lv_obj_get_parent(strip)==document_area&&lv_obj_get_parent(text)==document_area);
    lv_obj_get_coords(strip,&strip_area);lv_obj_get_coords(text,&text_area);
    assert(strip_area.x1==text_area.x1&&strip_area.x2==text_area.x2);
    assert(strip_area.y2+1==text_area.y1);
}
static void close_tab(unsigned index) {
    lv_obj_t *row=lv_obj_get_child(ui_tabs_object(tabs),index);
    lv_obj_t *close=lv_obj_get_child(row,1);
    assert(lv_obj_check_type(row,&lv_button_class)&&lv_obj_check_type(close,&lv_button_class));
    lv_obj_send_event(close,LV_EVENT_CLICKED,NULL);
}
static void syntax_workflow(const char *directory) {
    static const char javascript[]="const answer = 42;\nconsole.log(answer);\n";
    static const char rfl_source[]="fn main() -> i16 {\n    let answer = 42;\n    answer\n}\n";
    char text_path[2048],js_path[2048];
    unsigned long selection[2];
    ui_document *js,*rfl;
    TextBuffer *buffer;
    snprintf(text_path,sizeof(text_path),"%s/syntax.txt",directory);
    snprintf(js_path,sizeof(js_path),"%s/syntax.js",directory);
    dispatch(NEW,NULL);js=visible;buffer=ui_document_text(js);
    assert(!ui_editor_text(editor,javascript,sizeof(javascript)-1));
    ui_editor_select(editor,2,6);
    syntax_chosen(UI_LANGUAGE_JAVASCRIPT,NULL);
    assert(ui_document_text(js)==buffer&&ui_document_get_language(js)==UI_LANGUAGE_JAVASCRIPT);
    assert(ui_document_effective_language(js)==UI_LANGUAGE_JAVASCRIPT&&text_modified(buffer));
    ui_editor_selection(editor,selection);
    assert(selection[0]==2&&selection[1]==6&&ui_editor_cursor(editor)==6);
    dispatch(UNDO,NULL);assert(text_length(buffer)==0&&!text_modified(buffer));
    dispatch(REDO,NULL);assert(text_length(buffer)==sizeof(javascript)-1&&text_modified(buffer));
    assert(ui_document_get_language(js)==UI_LANGUAGE_JAVASCRIPT);
    dispatch(NEW,NULL);rfl=visible;assert(!ui_editor_text(editor,rfl_source,sizeof(rfl_source)-1));
    syntax_chosen(UI_LANGUAGE_RFL,NULL);
    assert(ui_document_get_language(rfl)==UI_LANGUAGE_RFL&&ui_document_get_language(js)==UI_LANGUAGE_JAVASCRIPT);
    remember_view();ui_documents_select(documents,js);show_document();
    dispatch(SAVE_AS,NULL);assert(path_dialog);settle_path();ui_path_set(path_dialog,text_path);ui_path_submit(path_dialog);
    assert(!ui_overlay_active()&&!text_modified(ui_document_text(js)));
    assert(ui_document_get_language(js)==UI_LANGUAGE_JAVASCRIPT&&ui_document_effective_language(js)==UI_LANGUAGE_JAVASCRIPT);
    dispatch(RELOAD,NULL);assert(!ui_overlay_active());
    assert(ui_document_get_language(js)==UI_LANGUAGE_JAVASCRIPT&&ui_document_effective_language(js)==UI_LANGUAGE_JAVASCRIPT);
    syntax_chosen(UI_LANGUAGE_AUTOMATIC,NULL);
    assert(ui_document_effective_language(js)==UI_LANGUAGE_TEXT&&!text_modified(ui_document_text(js)));
    dispatch(SAVE_AS,NULL);assert(path_dialog);settle_path();ui_path_set(path_dialog,js_path);ui_path_submit(path_dialog);
    assert(!ui_overlay_active()&&ui_document_get_language(js)==UI_LANGUAGE_AUTOMATIC);
    assert(ui_document_effective_language(js)==UI_LANGUAGE_JAVASCRIPT);
    dispatch(RELOAD,NULL);assert(ui_document_effective_language(js)==UI_LANGUAGE_JAVASCRIPT);
    assert(ui_document_get_language(rfl)==UI_LANGUAGE_RFL&&text_modified(ui_document_text(rfl)));
    editor_layout();
}
static void select_document(ui_document *document) {
    remember_view();assert(!ui_documents_select(documents,document));show_document();
}
static void append_text(const char *text) {
    unsigned long end=text_length(ui_document_text(visible));
    ui_editor_select(editor,end,end);assert(!ui_editor_text(editor,text,strlen(text)));
}
static ui_document *named_document(const char *path,const char *text) {
    ui_document *document;
    dispatch(NEW,NULL);document=visible;append_text(text);
    assert(!ui_document_save(documents,document,path,0));refresh();
    cancel_autosave(NULL);return document;
}
static void autosave_workflow(const char *directory) {
    char path[2048],close_path[2048],io_directory[2048],io_path[2048];
    struct gx_event focus;
    ui_document *named,*draft,*blank,*closing,*io;
    TextBuffer *buffer;
    unsigned long bounds[2],cursor,revision,generation;
    int32_t scroll_x,scroll_y;
    unsigned i;
    /* Begin a fresh workspace while retaining the same real app widgets. */
    ui_overlay_close();cancel_autosave(NULL);reset_operation();deferred=LATER_NONE;
    syncing=1;ui_editor_buffer(editor,NULL);visible=NULL;
    while(ui_documents_count(documents))assert(!ui_documents_close(documents,ui_documents_at(documents,0),1));
    assert(ui_documents_new(documents));syncing=0;show_document();
    snprintf(path,sizeof(path),"%s/autosave.txt",directory);
    snprintf(close_path,sizeof(close_path),"%s/autosave-close.txt",directory);
    snprintf(io_directory,sizeof(io_directory),"%s/autosave-io",directory);
    snprintf(io_path,sizeof(io_path),"%s/autosave-io/file.txt",directory);
    named=named_document(path,"disk");buffer=ui_document_text(named);
    append_text("-edited");ui_editor_select(editor,2,6);
    cursor=ui_editor_cursor(editor);revision=text_revision(buffer);generation=text_generation(buffer);
    scroll_x=lv_obj_get_scroll_x(ui_editor_object(editor));scroll_y=lv_obj_get_scroll_y(ui_editor_object(editor));
    /* A genuine textarea blur queues once and performs no callback-time I/O. */
    lv_group_focus_obj(file_menu);assert(autosave_count==1&&autosaves[0]==named);
    editor_blurred(NULL);assert(autosave_count==1);equals(path,"disk");
    workflow_tick(NULL);equals(path,"disk-edited");
    assert(!autosave_count&&!text_modified(buffer)&&ui_document_text(named)==buffer);
    assert(text_revision(buffer)==revision&&text_generation(buffer)==generation);
    assert(lv_group_get_focused(gx_lv_group())==file_menu&&ui_editor_cursor(editor)==cursor);
    ui_editor_selection(editor,bounds);assert(bounds[0]==2&&bounds[1]==6);
    assert(lv_obj_get_scroll_x(ui_editor_object(editor))==scroll_x&&lv_obj_get_scroll_y(ui_editor_object(editor))==scroll_y);
    dispatch(UNDO,NULL);assert(text_length(buffer)==4&&text_modified(buffer));
    dispatch(REDO,NULL);assert(text_length(buffer)==11&&!text_modified(buffer));
    /* Neither an unchanged named document nor a dirty untitled tab is saved. */
    lv_group_focus_obj(file_menu);assert(!autosave_count);workflow_tick(NULL);
    dispatch(NEW,NULL);draft=visible;append_text("untitled");lv_group_focus_obj(file_menu);
    workflow_tick(NULL);assert(!autosave_count&&!path_dialog&&!ui_overlay_active());
    assert(!*ui_document_path(draft)&&text_modified(ui_document_text(draft)));
    /* Keyboard-style tab replacement keeps the outgoing document as the job. */
    select_document(named);append_text("-tab");dispatch(NEW,NULL);blank=visible;
    assert(blank!=named&&autosave_count==1&&autosaves[0]==named);
    equals(path,"disk-edited");workflow_tick(NULL);equals(path,"disk-edited-tab");
    assert(visible==blank&&ui_editor_cursor(editor)==0&&text_length(ui_document_text(blank))==0);
    assert(!text_modified(ui_document_text(named))&&ui_editor_focused(editor));
    /* Host-window focus loss is observable without a toolkit focus change. */
    select_document(named);append_text("-focus");memset(&focus,0,sizeof(focus));
    focus.version=GX_VERSION;focus.type=GX_FOCUS;focus.flags=GX_FOCUSED;
    assert(!editor_event(&focus)&&!autosave_count);focus.flags=0;
    assert(!editor_event(&focus)&&autosave_count==1&&ui_editor_focused(editor));
    workflow_tick(NULL);equals(path,"disk-edited-tab-focus");assert(ui_editor_focused(editor));
    lv_group_focus_obj(file_menu);assert(!editor_event(&focus)&&!autosave_count);
    ui_editor_focus(editor);
    /* Save As cancellation cannot autosave over the original destination. */
    append_text("-saveas");editor_blurred(NULL);assert(autosave_count==1);
    dispatch(SAVE_AS,NULL);assert(path_dialog&&!autosave_count);
    workflow_tick(NULL);equals(path,"disk-edited-tab-focus");
    ui_path_cancel(path_dialog);workflow_tick(NULL);
    assert(text_modified(ui_document_text(named))&&!autosave_count&&!pending);
    equals(path,"disk-edited-tab-focus");
    editor_blurred(NULL);assert(autosave_count==1);dispatch(SAVE,NULL);
    assert(!autosave_count&&!text_modified(ui_document_text(named)));
    equals(path,"disk-edited-tab-focus-saveas");
    /* Reload/Discard must retain the disk version rather than save first. */
    append_text("-reload");editor_blurred(NULL);assert(autosave_count==1);
    dispatch(RELOAD,NULL);assert(ui_overlay_active()&&!autosave_count);
    choose("Discard");workflow_tick(NULL);
    equals(path,"disk-edited-tab-focus-saveas");assert(!text_modified(ui_document_text(named)));
    /* Close removes queued identities before freeing their buffers. */
    closing=named_document(close_path,"close disk");append_text("-local");editor_blurred(NULL);
    assert(autosave_count==1);request_close(closing);assert(ui_overlay_active()&&!autosave_count);
    choose("Discard");workflow_tick(NULL);workflow_tick(NULL);
    equals(close_path,"close disk");assert(!document_present(closing)&&!autosave_count);
    /* Quit and a later cancellation must not revive a discarded save job. */
    select_document(named);append_text("-quit");editor_blurred(NULL);assert(autosave_count==1);
    request_quit();assert(quitting&&ui_overlay_active()&&!autosave_count);
    choose("Discard");workflow_tick(NULL);workflow_tick(NULL);
    assert(ui_overlay_active());choose("Cancel");workflow_tick(NULL);
    assert(!quitting&&!autosave_count&&text_modified(ui_document_text(named)));
    equals(path,"disk-edited-tab-focus-saveas");
    /* Conflicts stay dirty and are consumed, not retried on every idle tick. */
    select_document(named);assert(!ui_document_reload(documents,named,1));show_document();
    append_text("-local");put_file(path,"external");lv_group_focus_obj(file_menu);
    assert(autosave_count==1);workflow_tick(NULL);equals(path,"external");
    assert(!autosave_count&&!pending&&!deferred&&!ui_overlay_active());
    assert(text_modified(ui_document_text(named))&&strstr(notice,"file changed on disk"));
    assert(!ui_document_reload(documents,named,1));show_document();append_text("-later");
    for(i=0;i<20;i++)workflow_tick(NULL);
    equals(path,"external");assert(!autosave_count&&text_modified(ui_document_text(named)));
    /* A non-directory parent produces a real I/O error even under root. */
    assert(!mkdir(io_directory,0700));io=named_document(io_path,"io disk");append_text("-local");
    assert(!unlink(io_path)&&!rmdir(io_directory));put_file(io_directory,"blocked");
    lv_group_focus_obj(file_menu);assert(autosave_count==1);workflow_tick(NULL);
    equals(io_directory,"blocked");assert(!autosave_count&&!pending&&!deferred&&!ui_overlay_active());
    assert(text_modified(ui_document_text(io))&&strstr(notice,"Autosave failed"));
    assert(!unlink(io_directory)&&!mkdir(io_directory,0700));put_file(io_path,"io disk");
    assert(!ui_document_reload(documents,io,1));show_document();append_text("-later");
    for(i=0;i<20;i++)workflow_tick(NULL);
    equals(io_path,"io disk");assert(!autosave_count&&text_modified(ui_document_text(io)));
    /* Stale queue entries are checked for membership before dereferencing. */
    autosaves[0]=(ui_document *)documents;autosave_count=1;workflow_tick(NULL);assert(!autosave_count);
    cancel_autosave(NULL);
}
int main(int argc,char **argv) {
    char path[2048],copy[2048],background[2048];
    ui_document *first,*second;
    unsigned long bounds[2];
    assert(argc==2);
    snprintf(path,sizeof(path),"%s/workflow.txt",argv[1]);
    snprintf(copy,sizeof(copy),"%s/copy.txt",argv[1]);
    snprintf(background,sizeof(background),"%s/background.txt",argv[1]);
    documents=ui_documents_create(argv[1]);assert(documents&&ui_documents_new(documents));
    assert(gx_lv_open("Editor workflow test",880,600)==0);
    lv_tick_set_cb(workflow_clock);
    actions=ui_actions_create(commands,sizeof(commands)/sizeof(commands[0]),dispatch,NULL);assert(actions);
    assert(create_ui());
    editor_layout();
    first=visible;
    assert(!ui_editor_text(editor,"Unicode: \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd1\x96\xd1\x82",21));
    dispatch(CLOSE_TAB,NULL);assert(ui_overlay_active());choose("Cancel");
    assert(!ui_overlay_active()&&ui_documents_count(documents)==1&&visible==first);
    dispatch(SAVE,NULL);assert(path_dialog);settle_path();
    ui_path_set(path_dialog,path);ui_path_submit(path_dialog);
    assert(!ui_overlay_active()&&!text_modified(ui_document_text(first)));
    editor_layout();assert(!strstr(lv_label_get_text(status),path));
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
    close_tab(1);choose("Cancel");assert(visible==first&&ui_documents_count(documents)==2);
    close_tab(1);choose("Discard");workflow_tick(NULL);assert(visible==first&&ui_documents_count(documents)==1);
    /* Saving a dirty background tab uses its buffer/path without selecting it. */
    dispatch(NEW,NULL);second=visible;assert(!ui_editor_text(editor,"background save",15));
    dispatch(SAVE,NULL);assert(path_dialog);settle_path();ui_path_set(path_dialog,background);ui_path_submit(path_dialog);
    assert(!ui_overlay_active());assert(!ui_editor_text(editor,"!",1));remember_view();
    ui_documents_select(documents,first);show_document();
    {
        TextBuffer *active_buffer=ui_document_text(first);
        unsigned long active_cursor=ui_editor_cursor(editor);
        close_tab(1);choose("Save");workflow_tick(NULL);
        /* Save completes first; deferred close runs on the following turn. */
        assert(deferred==LATER_CONTINUE&&visible==first&&ui_documents_count(documents)==2);
        assert(!text_modified(ui_document_text(second)));
        workflow_tick(NULL);
        assert(!ui_overlay_active()&&ui_documents_count(documents)==1&&visible==first);
        assert(ui_document_text(first)==active_buffer&&ui_editor_cursor(editor)==active_cursor);
        equals(background,"background save!");editor_layout();
    }
    /* Serial quit cancellation preserves all remaining buffers. */
    dispatch(NEW,NULL);second=visible;ui_editor_text(editor,"dirty",5);
    request_quit();assert(ui_overlay_active());choose("Discard");workflow_tick(NULL);workflow_tick(NULL);
    assert(ui_overlay_active());choose("Cancel");assert(!quitting&&ui_documents_count(documents)==2);
    syntax_workflow(argv[1]);
    autosave_workflow(argv[1]);
    ui_overlay_close();gx_lv_close();ui_actions_delete(actions);ui_documents_delete(documents);
    puts("EDITOR_WORKFLOW passed attached layout, shared tab close, dirty cancel/discard/save, syntax isolation/undo/Auto/save-as/reload, external-change copy, view retention, serial quit cancellation and deferred blur/tab/window autosave with explicit-operation/conflict/I/O safeguards");
    return 0;
}
