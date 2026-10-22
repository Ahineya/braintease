/* SPDX-License-Identifier: MIT */
#include "ui-app.h"
#include "ui-forms.h"
#include "ui-tabs.h"
#include "ui-editor.h"
#include "ui-document.h"
#include <gx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ui_documents *documents;
static ui_document *visible, *pending;
static ui_editor *editor;
static ui_tabs *tabs;
static ui_actions *actions;
static lv_obj_t *menubar, *document_area, *status, *file_menu, *edit_menu, *view_menu, *help_menu;
static ui_path *path_dialog;
static lv_timer_t *workflow;
static unsigned screen_width = 880, screen_height = 600;
static int trace, syncing, quitting, settings_warning;
static unsigned quit_index, continuation, deferred;
static ui_document *autosaves[UI_DOCUMENT_TABS];
static unsigned autosave_count;
static unsigned long last_generation;
static char notice[200];
static char *conflict_path;
enum { NEW = 1, OPEN, SAVE, SAVE_AS, RELOAD, CLOSE_TAB, QUIT, UNDO, REDO,
       CUT, COPY, PASTE, SELECT_ALL, WRAP, NEXT_TAB, PREVIOUS_TAB, ABOUT, CLEAR_RECENT,
       SYNTAX_LANGUAGE,
       RECENT_FIRST = 100 };
enum { CONT_NONE, CONT_CLOSE, CONT_QUIT, CONT_RELOAD };
enum { LATER_NONE, LATER_SAVE, LATER_SAVE_AS, LATER_CONTINUE, LATER_CONFLICT,
       LATER_ERROR, LATER_QUIT };
static const struct ui_action_spec commands[] = {
    {NEW,"New","Ctrl+N","file",17,1,1},
    {OPEN,"Open…","Ctrl+O","folder",18,1,1},
    {SAVE,"Save","Ctrl+S","save",22,1,1},
    {SAVE_AS,"Save as…","Ctrl+Shift+S","save",22,3,1},
    {RELOAD,"Reload from disk","F5","refresh-cw",62,0,1},
    {CLOSE_TAB,"Close tab","Ctrl+W","x",26,1,1},
    {QUIT,"Quit","Ctrl+Q","x",20,1,1},
    {UNDO,"Undo","Ctrl+Z","arrow-left",29,1,1},
    {REDO,"Redo","Ctrl+Shift+Z","arrow-right",29,3,1},
    {CUT,"Cut","Ctrl+X","file",27,1,1},
    {COPY,"Copy","Ctrl+C","file",6,1,1},
    {PASTE,"Paste","Ctrl+V","file",25,1,1},
    {SELECT_ALL,"Select all","Ctrl+A","file",4,1,1},
    {WRAP,"Wrap lines","","file",0,0,1},
    {NEXT_TAB,"Next tab","Ctrl+Tab","arrow-right",43,1,1},
    {PREVIOUS_TAB,"Previous tab","Ctrl+Shift+Tab","arrow-left",43,3,1},
    {ABOUT,"About Text Editor","F1","info",58,0,1},
    {CLEAR_RECENT,"Clear recent files","","trash",0,0,1},
    {SYNTAX_LANGUAGE,"Syntax language…","","file-code",0,0,1}
};
static void trace_rect(const char *name,lv_obj_t *object) {
    lv_area_t area;
    if(!trace||!object)return;
    lv_obj_update_layout(lv_screen_active());lv_obj_update_layout(lv_layer_top());
    lv_obj_get_coords(object,&area);
    fprintf(stderr,"EDIT_RECT %s %ld,%ld %ldx%ld density=%lu\n",name,(long)area.x1,(long)area.y1,
        (long)(area.x2-area.x1+1),(long)(area.y2-area.y1+1),(unsigned long)gx_lv_density());
}
static void trace_dialog(ui_dialog *dialog) {
    lv_obj_t *frame,*footer;
    unsigned i;
    if(!trace||!dialog)return;
    frame=lv_obj_get_parent(ui_dialog_body(dialog));
    trace_rect("dialog",frame);
    footer=lv_obj_get_child(frame,lv_obj_get_child_count(frame)-1);
    for(i=0;i<lv_obj_get_child_count(footer);i++) {
        lv_obj_t *button=lv_obj_get_child(footer,i);
        if(lv_obj_get_child_count(button))trace_rect(lv_label_get_text(lv_obj_get_child(button,0)),button);
    }
}
static void trace_tabs(void) {
    unsigned i,count,active=0;
    lv_obj_t *root;
    char name[32];
    if(!trace||!tabs)return;
    count=ui_documents_count(documents);root=ui_tabs_object(tabs);
    for(i=0;i<count;i++)if(ui_documents_at(documents,i)==visible)active=i+1;
    fprintf(stderr,"EDIT_TABS count=%lu active=%lu\n",(unsigned long)count,(unsigned long)active);
    for(i=0;i<count;i++) {
        ui_document *document=ui_documents_at(documents,i);
        lv_obj_t *tab=lv_obj_get_child(root,i);
        fprintf(stderr,"EDIT_DOCUMENT index=%lu active=%lu dirty=%lu bytes=%lu choice=%lu effective=%lu language=%s name=%s\n",
            (unsigned long)(i+1),(unsigned long)(document==visible),
            (unsigned long)text_modified(ui_document_text(document)),
            (unsigned long)text_length(ui_document_text(document)),
            (unsigned long)ui_document_get_language(document),
            (unsigned long)ui_document_effective_language(document),
            ui_document_language_name(document),ui_document_name(document));
        snprintf(name,sizeof(name),"tab-%lu",(unsigned long)(i+1));trace_rect(name,tab);
        if(tab&&lv_obj_get_child_count(tab)>1) {
            snprintf(name,sizeof(name),"tab-close-%lu",(unsigned long)(i+1));
            trace_rect(name,lv_obj_get_child(tab,1));
        }
    }
}
static void show_document(void);
static void refresh(void);
static void request_close(ui_document *document);
static void request_quit(void);
static void continue_operation(void);
static void begin_save(int save_as);
static void syntax_menu(void);
static int document_present(ui_document *document) {
    unsigned i;
    for(i=0;i<ui_documents_count(documents);i++)
        if(ui_documents_at(documents,i)==document)return 1;
    return 0;
}
static void cancel_autosave(ui_document *document) {
    unsigned i,j;
    if(!document){autosave_count=0;return;}
    for(i=0;i<autosave_count;i++)if(autosaves[i]==document) {
        for(j=i+1;j<autosave_count;j++)autosaves[j-1]=autosaves[j];
        autosave_count--;return;
    }
}
static void queue_autosave(ui_document *document) {
    unsigned i;
    if(syncing||pending||quitting||continuation||deferred||!document_present(document))return;
    if(!*ui_document_path(document)||!text_modified(ui_document_text(document)))return;
    for(i=0;i<autosave_count;i++)if(autosaves[i]==document)return;
    if(autosave_count<UI_DOCUMENT_TABS)autosaves[autosave_count++]=document;
}
static void editor_blurred(lv_event_t *event) {
    (void)event;
    queue_autosave(visible);
}
static int editor_event(const struct gx_event *event) {
    if(event->type==GX_FOCUS&&!(event->flags&GX_FOCUSED)&&ui_editor_focused(editor))
        queue_autosave(visible);
    return 0;
}
static void tell(const char *message) {
    snprintf(notice,sizeof(notice),"%s",message ? message : "");
    refresh();
}
static void remember_view(void) {
    unsigned long bounds[2], cursor, anchor;
    if (!visible || !editor) return;
    cursor=ui_editor_cursor(editor);ui_editor_selection(editor,bounds);
    anchor=cursor==bounds[0]?bounds[1]:bounds[0];
    ui_document_view(visible,cursor,anchor);
}
static void save_preferences(void) {
    settings_warning=ui_documents_settings_save(documents)<0;
}
static void autosave_tick(void) {
    ui_document *document;
    int result;
    if(!autosave_count||syncing||pending||quitting||continuation||deferred)return;
    document=autosaves[0];cancel_autosave(document);
    if(!document_present(document)||!*ui_document_path(document)||
       !text_modified(ui_document_text(document)))return;
    /* Save outside focus callbacks without entering the manual-save workflow.
     * Menus may stay open; explicit save/discard operations take precedence. */
    result=ui_document_save(documents,document,NULL,0);
    if(result==UI_DOCUMENT_OK) {
        notice[0]=0;
        if(document==visible)last_generation=text_generation(ui_document_text(document));
        save_preferences();
        if(trace)fprintf(stderr,"EDIT_AUTOSAVE %s\n",ui_document_path(document));
    } else {
        snprintf(notice,sizeof(notice),"Autosave failed for %s: %s",ui_document_name(document),
            result==UI_DOCUMENT_CHANGED?"file changed on disk; use Save to resolve":ui_documents_error(documents));
        if(trace)fprintf(stderr,"EDIT_AUTOSAVE_FAILED %s result=%ld\n",ui_document_path(document),(long)result);
    }
    refresh();
}
static void refresh(void) {
    struct ui_tab_spec spec[UI_DOCUMENT_TABS];
    unsigned i, count=ui_documents_count(documents), selected=1;
    unsigned long cursor=editor?ui_editor_cursor(editor):0, row=0, column=0;
    char caption[300];
    TextBuffer *buffer=ui_document_text(visible);
    if (!tabs || !visible) return;
    for(i=0;i<count;i++) {
        ui_document *d=ui_documents_at(documents,i);
        spec[i]=(struct ui_tab_spec){i+1,ui_document_name(d),text_modified(ui_document_text(d)),1};
        if(d==visible)selected=i+1;
    }
    ui_tabs_bind(tabs,spec,count,selected);
    if(buffer) { row=text_row(buffer,cursor);column=text_visual(buffer,cursor,4); }
    snprintf(caption,sizeof(caption),"%lu:%lu  |  %s  |  %s  |  %lu bytes%s%s%s",
        row+1,column+1,ui_document_language_name(visible),ui_documents_wrap(documents)?"Wrap on":"Wrap off",
        buffer?text_length(buffer):0,notice[0]?"  |  ":"",notice,
        settings_warning?"  |  Settings could not be saved":"");
    lv_label_set_text(status,caption);
    ui_actions_enable(actions,RELOAD,*ui_document_path(visible)!=0);
    ui_actions_enable(actions,CLEAR_RECENT,ui_documents_recent_count(documents)!=0);
    ui_actions_enable(actions,NEXT_TAB,count>1);
    ui_actions_enable(actions,PREVIOUS_TAB,count>1);
    trace_tabs();
}
static void show_document(void) {
    unsigned long cursor, anchor;
    if(visible!=ui_documents_active(documents))queue_autosave(visible);
    visible=ui_documents_active(documents);
    if(!visible)visible=ui_documents_new(documents);
    if(!visible){ui_message("Cannot create document",ui_documents_error(documents));return;}
    syncing=1;
    ui_editor_buffer(editor,ui_document_text(visible));
    ui_document_get_view(visible,&cursor,&anchor);
    ui_editor_select(editor,anchor,cursor);
    ui_editor_wrap(editor,ui_documents_wrap(documents));
    last_generation=text_generation(ui_document_text(visible));
    syncing=0;notice[0]=0;refresh();ui_editor_focus(editor);
    if(trace)fprintf(stderr,"EDIT_TAB name=%s dirty=%lu bytes=%lu\n",ui_document_name(visible),
        (unsigned long)text_modified(ui_document_text(visible)),text_length(ui_document_text(visible)));
}
static void edited(lv_event_t *event) {
    unsigned long generation;
    (void)event;
    if(syncing||!visible)return;
    generation=text_generation(ui_document_text(visible));
    if(generation!=last_generation) {
        notice[0]=0;
        if(trace)fprintf(stderr,"EDIT_CHANGE bytes=%lu dirty=%lu\n",
            text_length(ui_document_text(visible)),(unsigned long)text_modified(ui_document_text(visible)));
        last_generation=generation;
    }
    if(*ui_editor_error(editor))snprintf(notice,sizeof(notice),"%s",ui_editor_error(editor));
    remember_view();refresh();
}
static void tab_event(unsigned id,int close,void *unused) {
    ui_document *d=ui_documents_at(documents,id?id-1:UI_DOCUMENT_TABS);
    (void)unused;
    if(!d||ui_overlay_active())return;
    if(close){request_close(d);return;}
    remember_view();ui_documents_select(documents,d);show_document();
}
static void open_file(const char *path) {
    remember_view();
    if(!ui_documents_open(documents,path)) {ui_message("Cannot open file",ui_documents_error(documents));return;}
    show_document();save_preferences();
    if(trace)fprintf(stderr,"EDIT_OPEN %s\n",ui_document_path(visible));
}
static void reset_operation(void) {pending=NULL;continuation=CONT_NONE;quitting=0;quit_index=0;free(conflict_path);conflict_path=NULL;}
static int open_accept(ui_path *dialog,const char *path,void *unused) {
    (void)unused;
    if(!path){path_dialog=NULL;return 1;}
    remember_view();
    if(!ui_documents_open(documents,path)) {ui_path_error(dialog,ui_documents_error(documents));return 0;}
    show_document();save_preferences();path_dialog=NULL;
    if(trace)fprintf(stderr,"EDIT_OPEN %s\n",ui_document_path(visible));
    return 1;
}
static const char *open_initial(void) {
    const char *path=ui_document_path(visible);
    if(*path)return path;
    if(ui_documents_recent_count(documents))return ui_documents_recent(documents,0);
    return "";
}
static char *save_initial(void) {
    const char *path=ui_documents_recent_count(documents)?ui_documents_recent(documents,0):NULL;
    const char *home=getenv("HOME"),*slash;
    size_t directory=0;
    char *initial;
    if(path&&(slash=strrchr(path,'/')))directory=(size_t)(slash-path)+1;
    else if(home&&*home=='/')directory=strlen(home)+(home[strlen(home)-1]=='/'?0:1);
    if(!directory||directory+13>=UI_DOCUMENT_PATH)return NULL;
    initial=malloc(directory+13);
    if(!initial)return NULL;
    if(path)memcpy(initial,path,directory);
    else {
        strcpy(initial,home);
        if(directory>strlen(home))initial[directory-1]='/';
    }
    strcpy(initial+directory,"Untitled.txt");
    return initial;
}
static void open_dialog(void) {
    path_dialog=ui_path_open("Open text file",UI_PATH_OPEN,open_initial(),open_accept,NULL);
    if(trace)fprintf(stderr,"EDIT_DIALOG open\n");
    if(path_dialog){trace_rect("open-input",ui_path_input(path_dialog));if(ui_path_list(path_dialog))trace_rect("open-list",ui_table_object(ui_path_list(path_dialog)));}
}
static void saved(void) {
    free(conflict_path);conflict_path=NULL;
    save_preferences();
    if(pending==visible){last_generation=text_generation(ui_document_text(visible));refresh();}
    if(trace)fprintf(stderr,"EDIT_SAVE %s\n",ui_document_path(pending));
    if(continuation)deferred=LATER_CONTINUE;
    else {pending=NULL;tell("Saved");}
}
static int save_accept(ui_path *dialog,const char *path,void *unused) {
    int result;
    (void)unused;
    if(!path){path_dialog=NULL;reset_operation();return 1;}
    result=ui_document_save(documents,pending,path,UI_DOCUMENT_REPLACE);
    if(result==UI_DOCUMENT_CHANGED){
        char *copy=malloc(strlen(path)+1);
        if(!copy){ui_path_error(dialog,"Not enough memory.");return 0;}
        strcpy(copy,path);free(conflict_path);conflict_path=copy;
        path_dialog=NULL;deferred=LATER_CONFLICT;return 1;
    }
    if(result<0){ui_path_error(dialog,ui_documents_error(documents));return 0;}
    if(result==UI_DOCUMENT_EXISTS){ui_path_error(dialog,"Confirm replacement before saving.");return 0;}
    path_dialog=NULL;saved();return 1;
}
static void begin_save(int save_as) {
    int result;
    if(!pending)pending=visible;
    cancel_autosave(pending);
    if(save_as||!*ui_document_path(pending)) {
        char *initial=*ui_document_path(pending)?NULL:save_initial();
        path_dialog=ui_path_open("Save text file",UI_PATH_SAVE,initial?initial:ui_document_path(pending),save_accept,NULL);
        free(initial);
        if(trace)fprintf(stderr,"EDIT_DIALOG save\n");
        if(path_dialog){trace_rect("save-input",ui_path_input(path_dialog));if(ui_path_list(path_dialog))trace_rect("save-list",ui_table_object(ui_path_list(path_dialog)));}
        return;
    }
    result=ui_document_save(documents,pending,NULL,0);
    if(result==UI_DOCUMENT_CHANGED){deferred=LATER_CONFLICT;return;}
    if(result<0){snprintf(notice,sizeof(notice),"%s",ui_documents_error(documents));deferred=LATER_ERROR;return;}
    saved();
}
static int confirm_action(ui_dialog *dialog,unsigned id,void *unused) {
    (void)dialog;(void)unused;
    if(trace)fprintf(stderr,"EDIT_CONFIRM decision=%lu operation=%lu\n",(unsigned long)id,(unsigned long)continuation);
    cancel_autosave(pending);
    if(id==1)deferred=LATER_SAVE;
    else if(id==2)deferred=LATER_CONTINUE;
    else {reset_operation();deferred=LATER_NONE;}
    return 1;
}
static void ask_save(void) {
    ui_dialog *dialog=ui_dialog_open("Save changes?",440,confirm_action,NULL);
    lv_obj_t *label;
    char caption[200];
    if(!dialog){reset_operation();return;}
    snprintf(caption,sizeof(caption),"Save changes to “%s” before %s?",ui_document_name(pending),
        continuation==CONT_QUIT?"quitting":continuation==CONT_RELOAD?"reloading":"closing");
    label=ui_label(ui_dialog_body(dialog),caption,UI_BODY);lv_obj_set_width(label,lv_pct(100));
    ui_dialog_button(dialog,"Cancel",0,0);
    ui_dialog_button(dialog,"Discard",0,2);
    lv_group_focus_obj(ui_dialog_button(dialog,"Save",1,1));
    if(trace)fprintf(stderr,"EDIT_DIALOG unsaved operation=%lu name=%s\n",(unsigned long)continuation,ui_document_name(pending));
    trace_dialog(dialog);
}
static int conflict_action(ui_dialog *dialog,unsigned id,void *unused) {
    int result;
    (void)unused;
    if(id==1) {
        result=ui_document_save(documents,pending,conflict_path,UI_DOCUMENT_FORCE|UI_DOCUMENT_REPLACE);
        if(result==UI_DOCUMENT_CHANGED) {
            ui_label(ui_dialog_body(dialog),"The file changed again. Try saving a copy.",UI_ERROR);return 0;
        }
        if(result<0){ui_label(ui_dialog_body(dialog),ui_documents_error(documents),UI_ERROR);return 0;}
        saved();
    } else if(id==2)deferred=LATER_SAVE_AS;
    else reset_operation();
    if(trace)fprintf(stderr,"EDIT_CONFLICT decision=%lu\n",(unsigned long)id);
    return 1;
}
static void ask_conflict(void) {
    ui_dialog *dialog=ui_dialog_open("File changed on disk",460,conflict_action,NULL);
    lv_obj_t *label;
    if(!dialog){reset_operation();return;}
    label=ui_label(ui_dialog_body(dialog),"This file was changed, replaced or removed outside Text Editor. Keep both versions by saving a copy, or overwrite the disk version.",UI_BODY);
    lv_obj_set_width(label,lv_pct(100));
    ui_dialog_button(dialog,"Cancel",0,0);
    ui_dialog_button(dialog,"Overwrite",0,1);
    lv_group_focus_obj(ui_dialog_button(dialog,"Save a copy…",1,2));
    if(trace)fprintf(stderr,"EDIT_DIALOG conflict\n");
    trace_dialog(dialog);
}
static void continue_operation(void) {
    unsigned operation=continuation;
    if(operation==CONT_CLOSE) {
        ui_document *closed=pending;
        int was_visible=closed==visible;
        cancel_autosave(closed);
        /* Detach a borrowed buffer before its document releases it. */
        if(was_visible){syncing=1;ui_editor_buffer(editor,NULL);syncing=0;visible=NULL;}
        ui_documents_close(documents,closed,1);reset_operation();
        if(was_visible)show_document();else refresh();
        if(trace)fprintf(stderr,"EDIT_CLOSE count=%lu\n",(unsigned long)ui_documents_count(documents));
    } else if(operation==CONT_RELOAD) {
        TextBuffer *old=ui_document_text(pending);
        if(pending==visible){remember_view();syncing=1;ui_editor_buffer(editor,NULL);syncing=0;}
        if(ui_document_reload(documents,pending,1)<0) {
            if(pending==visible){
                unsigned long cursor,anchor;
                syncing=1;ui_editor_buffer(editor,old);
                ui_document_get_view(pending,&cursor,&anchor);
                ui_editor_select(editor,anchor,cursor);syncing=0;
            }
            ui_message("Cannot reload file",ui_documents_error(documents));reset_operation();refresh();return;
        }
        reset_operation();show_document();tell("Reloaded");
        if(trace)fprintf(stderr,"EDIT_RELOAD\n");
    } else if(operation==CONT_QUIT) {
        quit_index++;pending=NULL;
        deferred=LATER_QUIT;
    }
}
static void quit_next(void) {
    unsigned count=ui_documents_count(documents);
    while(quit_index<count) {
        pending=ui_documents_at(documents,quit_index);
        if(text_modified(ui_document_text(pending))){continuation=CONT_QUIT;ask_save();return;}
        quit_index++;
    }
    save_preferences();
    if(trace)fprintf(stderr,"EDIT_QUIT\n");
    gx_lv_finish();
}
static void request_close(ui_document *document) {
    if(ui_overlay_active())return;
    cancel_autosave(document);
    remember_view();pending=document;continuation=CONT_CLOSE;
    if(text_modified(ui_document_text(document)))ask_save();else continue_operation();
}
static void request_quit(void) {
    if(quitting||ui_overlay_active())return;
    cancel_autosave(NULL);
    remember_view();quitting=1;quit_index=0;quit_next();
}
static int close_request(void) {request_quit();return 0;}
static void workflow_tick(lv_timer_t *timer) {
    unsigned next;
    (void)timer;
    autosave_tick();
    if(ui_overlay_active()||!deferred)return;
    next=deferred;deferred=0;
    if(next==LATER_SAVE)begin_save(0);
    else if(next==LATER_SAVE_AS)begin_save(1);
    else if(next==LATER_CONTINUE)continue_operation();
    else if(next==LATER_CONFLICT)ask_conflict();
    else if(next==LATER_ERROR){ui_message("Cannot save file",notice);reset_operation();refresh();}
    else if(next==LATER_QUIT)quit_next();
}
static void dispatch(unsigned id,void *unused) {
    unsigned i,count;
    long position;
    TextBuffer *buffer=ui_document_text(visible);
    (void)unused;
    if(deferred||quitting)return;
    if(id>=RECENT_FIRST) {open_file(ui_documents_recent(documents,id-RECENT_FIRST));return;}
    switch(id) {
    case NEW:
        remember_view();if(!ui_documents_new(documents))ui_message("Cannot create document",ui_documents_error(documents));else show_document();break;
    case OPEN:open_dialog();break;
    case SAVE:pending=visible;begin_save(0);break;
    case SAVE_AS:pending=visible;begin_save(1);break;
    case RELOAD:cancel_autosave(visible);remember_view();pending=visible;continuation=CONT_RELOAD;if(text_modified(buffer))ask_save();else continue_operation();break;
    case CLOSE_TAB:request_close(visible);break;
    case QUIT:request_quit();break;
    case UNDO:case REDO:
        position=id==UNDO?text_undo(buffer):text_redo(buffer);
        if(position>=0){ui_editor_changed(editor);ui_editor_select(editor,position,position);refresh();}
        else tell(id==UNDO?"Nothing to undo":"Nothing to redo");
        ui_editor_focus(editor);break;
    case CUT:case COPY:case PASTE:case SELECT_ALL:
        ui_editor_focus(editor);ui_editor_action(editor,id==CUT?GX_ACTION_CUT:id==COPY?GX_ACTION_COPY:id==PASTE?GX_ACTION_PASTE:GX_ACTION_SELECT_ALL);refresh();break;
    case WRAP:
        ui_documents_set_wrap(documents,!ui_documents_wrap(documents));ui_editor_wrap(editor,ui_documents_wrap(documents));save_preferences();refresh();break;
    case NEXT_TAB:case PREVIOUS_TAB:
        count=ui_documents_count(documents);for(i=0;i<count;i++)if(ui_documents_at(documents,i)==visible)break;
        if(count){remember_view();ui_documents_select(documents,ui_documents_at(documents,(i+count+(id==NEXT_TAB?1:-1))%count));show_document();}break;
    case CLEAR_RECENT:ui_documents_clear_recent(documents);save_preferences();refresh();break;
    case SYNTAX_LANGUAGE:syntax_menu();break;
    case ABOUT:ui_message("Text Editor","A Graphite editor built on Rift’s text engine. Open files in tabs, edit Unicode text, and use Ctrl+S to save. Wrapping changes only the display; file contents stay intact. Ctrl+Tab switches tabs. Files may contain up to 512 KiB.");break;
    }
}
static void menu_dispatch(unsigned id,void *unused) {(void)unused;if(id>=RECENT_FIRST)dispatch(id,NULL);else ui_actions_invoke(actions,id);}
static void file_items(lv_event_t *event) {
    struct ui_menu_item items[UI_DOCUMENT_RECENTS+9];
    unsigned i,n=0;
    static const unsigned first[]={NEW,OPEN,SAVE,SAVE_AS,RELOAD};
    (void)event;
    for(i=0;i<5;i++)ui_actions_item(actions,first[i],&items[n++]);
    for(i=0;i<ui_documents_recent_count(documents);i++)items[n++]=(struct ui_menu_item){ui_documents_recent(documents,i),"","file",RECENT_FIRST+i,0};
    ui_actions_item(actions,CLEAR_RECENT,&items[n++]);ui_actions_item(actions,CLOSE_TAB,&items[n++]);ui_actions_item(actions,QUIT,&items[n++]);
    ui_menu_open(file_menu,items,n,menu_dispatch,NULL);
}
static void edit_items(lv_event_t *event) {static const unsigned ids[]={UNDO,REDO,CUT,COPY,PASTE,SELECT_ALL};(void)event;ui_actions_menu(actions,edit_menu,ids,6);}
static void view_items(lv_event_t *event) {static const unsigned ids[]={WRAP,SYNTAX_LANGUAGE,NEXT_TAB,PREVIOUS_TAB};(void)event;ui_actions_menu(actions,view_menu,ids,4);}
static void help_items(lv_event_t *event) {static const unsigned ids[]={ABOUT};(void)event;ui_actions_menu(actions,help_menu,ids,1);}
static void syntax_chosen(unsigned language,void *unused) {
    (void)unused;
    if(ui_document_set_language(visible,language))return;
    /* Syntax invalidates painting without replacing the buffer or its view. */
    last_generation=text_generation(ui_document_text(visible));
    ui_editor_changed(editor);ui_editor_focus(editor);
    if(trace)fprintf(stderr,"EDIT_LANGUAGE choice=%lu effective=%lu name=%s\n",
        (unsigned long)ui_document_get_language(visible),
        (unsigned long)ui_document_effective_language(visible),ui_document_language_name(visible));
}
static void syntax_menu(void) {
    struct ui_menu_item items[UI_LANGUAGE_COUNT];
    unsigned i,count=ui_document_language_count(),selected=ui_document_get_language(visible);
    lv_obj_t *first,*frame,*row;
    char name[32];
    for(i=0;i<count;i++) {
        const struct ui_document_language *language=ui_document_language_at(i);
        items[i]=(struct ui_menu_item){language->name,language->id==selected?"Selected":"",
            "file-code",language->id,0};
    }
    if(!ui_menu_open(view_menu,items,count,syntax_chosen,NULL))return;
    first=lv_group_get_focused(gx_lv_group());
    if(first) {
        frame=lv_obj_get_parent(first);
        row=lv_obj_get_child(frame,selected);
        lv_group_focus_obj(row);lv_obj_scroll_to_view(row,LV_ANIM_OFF);
        if(trace) {
            trace_rect("syntax-menu",frame);
            for(i=0;i<count;i++) {
                snprintf(name,sizeof(name),"syntax-%lu",(unsigned long)i);
                trace_rect(name,lv_obj_get_child(frame,i));
            }
        }
    }
    if(trace)fprintf(stderr,"EDIT_LANGUAGE_MENU selected=%lu effective=%lu\n",
        (unsigned long)selected,(unsigned long)ui_document_effective_language(visible));
}
static void menu_key(lv_event_t *event) {
    lv_obj_t *obj=lv_event_get_target_obj(event);
    if(lv_event_get_key(event)!=LV_KEY_DOWN)return;
    if(obj==file_menu)file_items(event);else if(obj==edit_menu)edit_items(event);else if(obj==view_menu)view_items(event);else help_items(event);
}
static int shortcuts(unsigned key,int down,unsigned modifiers) {
    if(ui_actions_key(actions,key,down,modifiers))return 1;
    if(ui_overlay_active())return 0;
    if(key==67){if(down)lv_group_focus_obj(file_menu);return 1;}
    return ui_editor_focused(editor)?ui_editor_key(editor,key,down,modifiers):0;
}
static void text(uint32_t scalar) {
    if(ui_editor_focused(editor)&&ui_editor_codepoint(editor,scalar)<0)tell(ui_editor_error(editor));
}
static void clipboard(unsigned action) {if(ui_editor_focused(editor)){ui_editor_action(editor,action);refresh();}}
static void resize(unsigned width,unsigned height) {
    screen_width=width;screen_height=height;
    gx_lv_height(menubar,UI_CONTROL);gx_lv_height(status,20);
    ui_tabs_density(tabs);ui_editor_density(editor);
    if(path_dialog&&ui_overlay_active())ui_path_density(path_dialog);
    ui_data_resize();
    trace_rect("menubar",menubar);
    trace_rect("File",file_menu);trace_rect("Edit",edit_menu);
    trace_rect("View",view_menu);trace_rect("Help",help_menu);
    trace_rect("document-area",document_area);
    trace_rect("editor",ui_editor_object(editor));
    trace_rect("tabs",ui_tabs_object(tabs));
    trace_rect("status",status);
    trace_tabs();
}
static int create_ui(void) {
    lv_obj_t *screen=lv_screen_active(),*object;
    ui_screen();lv_obj_set_flex_flow(screen,LV_FLEX_FLOW_COLUMN);
    menubar=ui_stack(screen,LV_FLEX_FLOW_ROW,UI_SPACE);
    file_menu=ui_button(menubar,"File",UI_TOOL);edit_menu=ui_button(menubar,"Edit",UI_TOOL);
    view_menu=ui_button(menubar,"View",UI_TOOL);help_menu=ui_button(menubar,"Help",UI_TOOL);
    lv_obj_add_event_cb(file_menu,file_items,LV_EVENT_CLICKED,NULL);lv_obj_add_event_cb(edit_menu,edit_items,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(view_menu,view_items,LV_EVENT_CLICKED,NULL);lv_obj_add_event_cb(help_menu,help_items,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(file_menu,menu_key,LV_EVENT_KEY,NULL);lv_obj_add_event_cb(edit_menu,menu_key,LV_EVENT_KEY,NULL);
    lv_obj_add_event_cb(view_menu,menu_key,LV_EVENT_KEY,NULL);lv_obj_add_event_cb(help_menu,menu_key,LV_EVENT_KEY,NULL);
    document_area=ui_stack(screen,LV_FLEX_FLOW_COLUMN,0);
    lv_obj_set_height(document_area,0);lv_obj_set_flex_grow(document_area,1);
    tabs=ui_tabs_create(document_area,tab_event,NULL);if(!tabs)return 0;
    editor=ui_editor_create(document_area,ui_document_text(ui_documents_active(documents)));if(!editor)return 0;
    object=ui_editor_object(editor);lv_obj_set_width(object,lv_pct(100));lv_obj_set_height(object,0);lv_obj_set_flex_grow(object,1);
    lv_obj_add_event_cb(object,edited,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(object,editor_blurred,LV_EVENT_DEFOCUSED,NULL);
    status=ui_label(screen,"",UI_SECONDARY);lv_obj_set_width(status,lv_pct(100));lv_label_set_long_mode(status,LV_LABEL_LONG_MODE_DOTS);
    show_document();resize(screen_width,screen_height);return 1;
}
int main(int argc,char **argv) {
    int i,code;
    documents=ui_documents_create(getenv("HOME"));
    if(!documents){fputs("Text Editor: not enough memory.\n",stderr);return 1;}
    for(i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--trace"))trace=1;
        else if(!strcmp(argv[i],"--help")){
            puts("Usage: lvedit [--trace] [FILE …]\nGraphite Text Editor. Ctrl+N New; Ctrl+O Open; Ctrl+S Save; Ctrl+Shift+S Save As; Ctrl+W Close tab; Ctrl+Q Quit. Ctrl+Tab switches tabs.\nCtrl+A/C/X/V Select/Copy/Cut/Paste; Ctrl+Z Undo; Ctrl+Shift+Z Redo; F5 Reload; F10 Menus.");
            ui_documents_delete(documents);return 0;
        } else if(!ui_documents_open(documents,argv[i])) {
            fprintf(stderr,"Text Editor: %s: %s\n",argv[i],ui_documents_error(documents));
            ui_documents_delete(documents);return 1;
        }
    }
    if(!ui_documents_count(documents)&&!ui_documents_new(documents)){ui_documents_delete(documents);return 1;}
    gx_lv_application("org.riplix.TextEditor", "Text Editor", "file-code");
    gx_lv_minimum(540,360);
    if(gx_lv_open("Text Editor",screen_width,screen_height)<0){ui_documents_delete(documents);return 1;}
    actions=ui_actions_create(commands,sizeof(commands)/sizeof(commands[0]),dispatch,NULL);
    if(!actions||!create_ui()){fputs("Text Editor: cannot create interface.\n",stderr);gx_lv_close();ui_actions_delete(actions);ui_documents_delete(documents);return 1;}
    workflow=lv_timer_create(workflow_tick,20,NULL);
    gx_lv_handlers(text,clipboard,resize);gx_lv_shortcuts(shortcuts);gx_lv_close_request(close_request);
    gx_lv_events(editor_event);
    if(trace)fprintf(stderr,"EDIT_READY tabs=%lu name=%s\n",(unsigned long)ui_documents_count(documents),ui_document_name(visible));
    code=gx_lv_run();
    gx_lv_events(NULL);
    gx_lv_close_request(NULL);
    if(workflow)lv_timer_delete(workflow);
    syncing=1;cancel_autosave(NULL);
    save_preferences();free(conflict_path);
    gx_lv_close();ui_actions_delete(actions);ui_documents_delete(documents);
    return code;
}
