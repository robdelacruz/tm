#include <gtk/gtk.h>
#include "clib.h"

GtkWidget *create_label(char *caption);
GtkWidget *create_label2(char *caption);
GtkWidget *create_markup_label(char *markup);
GtkWidget *create_markup_label2(char *markup);
void set_widget_margins(GtkWidget *w, int left, int right, int top, int bottom);

int GtkListBox_numrows(GtkWidget *lb);
void GtkListBox_append(GtkWidget *lb, char *text);
void GtkListBox_replace(GtkWidget *lb, int index, char *text);
void GtkListBox_remove(GtkWidget *lb, int index);

char *GtkTextView_gettext(GtkTextView *tv);

