#include <gtk/gtk.h>
#include "clib.h"

GtkWidget *create_label(char *caption);

int GtkListBox_numrows(GtkWidget *lb);
void GtkListBox_append(GtkWidget *lb, char *text);
void GtkListBox_replace(GtkWidget *lb, int index, char *text);
void GtkListBox_remove(GtkWidget *lb, int index);

