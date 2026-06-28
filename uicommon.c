#include <assert.h>

#include <gtk/gtk.h>
#include "uicommon.h"

GtkWidget *create_label(char *caption) {
    GtkWidget *lbl = gtk_label_new(caption);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_END);
    return lbl;
}
GtkWidget *create_label2(char *caption) {
    GtkWidget *lbl = gtk_label_new(caption);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    return lbl;
}
GtkWidget *create_markup_label(char *markup) {
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_END);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    return lbl;
}
GtkWidget *create_markup_label2(char *markup) {
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    return lbl;
}

void set_widget_margins(GtkWidget *w, int left, int right, int top, int bottom) {
    gtk_widget_set_margin_start(w, left);
    gtk_widget_set_margin_end(w, right);
    gtk_widget_set_margin_top(w, top);
    gtk_widget_set_margin_bottom(w, bottom);
}

void clear_controls(GtkWidget *w) {
    gtk_container_foreach(GTK_CONTAINER(w), (GtkCallback) gtk_widget_destroy, NULL);
}

int GtkListBox_numrows(GtkWidget *lb) {
    for (int i=0; ; i++) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), i);
        if (row == NULL)
            return i;
    }
    assert(FALSE);
    return 0;
}
void GtkListBox_append(GtkWidget *lb, char *text) {
    GtkWidget *lbl = create_label2(text);
    gtk_container_add(GTK_CONTAINER(lb), lbl);
}
void GtkListBox_replace(GtkWidget *lb, int index, char *text) {
    GtkWidget *row = (GtkWidget *) gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), index);
    if (row == NULL)
        return;
    gtk_container_remove(GTK_CONTAINER(lb), row);
    GtkWidget *lbl = create_label(text);
    gtk_list_box_insert(GTK_LIST_BOX(lb), lbl, index);
}
void GtkListBox_remove(GtkWidget *lb, int index) {
    GtkWidget *row = (GtkWidget *) gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), index);
    if (row)
        gtk_container_remove(GTK_CONTAINER(lb), row);
}

char *GtkTextView_gettext(GtkTextView *tv) {
    GtkTextBuffer *tb = gtk_text_view_get_buffer(tv);
    GtkTextIter start, end;
    gtk_text_buffer_get_start_iter(tb, &start);
    gtk_text_buffer_get_end_iter(tb, &end);
    return gtk_text_buffer_get_text(tb, &start, &end, FALSE);
}

