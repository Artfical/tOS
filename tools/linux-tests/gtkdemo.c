/* A small GTK 3 program that uses what Firefox needs from the toolkit: a menu bar with popup menus, a tooltip, a combo box
 * (its list is a popup window), a text entry, a check button, a file chooser, a scrolled list and a timer.
 *   gcc gtkdemo.c -o gtkdemo $(pkg-config --cflags --libs gtk+-3.0)   then   tos-x gtkdemo */
#include <gtk/gtk.h>

static GtkWidget *label;
static int ticks;

static gboolean tick(gpointer d) { char b[64]; snprintf(b, sizeof b, "timer: %d", ++ticks); gtk_label_set_text(GTK_LABEL(label), b); return G_SOURCE_CONTINUE; }
static void on_open(GtkMenuItem *m, gpointer win)
{
    (void)m;
    GtkWidget *d = gtk_file_chooser_dialog_new("Open a file", GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}
static void on_quit(GtkMenuItem *m, gpointer d) { (void)m; (void)d; gtk_main_quit(); }
static void on_click(GtkButton *b, gpointer d) { (void)b; (void)d; g_print("clicked\n"); }

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "GTK 3 on tOS");
    gtk_window_set_default_size(GTK_WINDOW(win), 520, 380);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(win), box);

    GtkWidget *bar = gtk_menu_bar_new(), *file = gtk_menu_item_new_with_label("File"), *menu = gtk_menu_new();
    GtkWidget *open = gtk_menu_item_new_with_label("Open..."), *quit = gtk_menu_item_new_with_label("Quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file);
    g_signal_connect(open, "activate", G_CALLBACK(on_open), win);
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit), NULL);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "type here");
    gtk_widget_set_tooltip_text(entry, "A tooltip: its own small window");
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 1; i <= 5; i++) { char b[32]; snprintf(b, sizeof b, "Choice %d", i); gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), b); }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    GtkWidget *check = gtk_check_button_new_with_label("A check button");
    GtkWidget *button = gtk_button_new_with_label("Click me");
    g_signal_connect(button, "clicked", G_CALLBACK(on_click), NULL);
    gtk_box_pack_start(GTK_BOX(row), entry, TRUE, TRUE, 4);
    gtk_box_pack_start(GTK_BOX(row), combo, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 4);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL), *view = gtk_text_view_new();
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), "A text view in a scrolled window.\nÇ ğ ı ö ş ü: accents and Pango text shaping.\n", -1);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 4);
    label = gtk_label_new("timer: 0");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 4);
    g_timeout_add(500, tick, NULL);
    gtk_widget_show_all(win);
    gtk_main();
    return 0;
}
