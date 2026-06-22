#include <assert.h>

#include <gtk/gtk.h>
#include "uicommon.h"

GtkWidget *create_label(char *caption) {
    GtkWidget *lbl = gtk_label_new(caption);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_END);
    return lbl;
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
    GtkWidget *lbl = create_label(text);
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


