#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <cstdlib>
#include <iostream>

static void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer) {
    const char* uri = webkit_web_view_get_uri(view);
    std::cerr << "[native-shell load] event=" << static_cast<int>(event) << " uri=" << (uri ? uri : "") << "\n";
}

static gboolean on_load_failed(WebKitWebView*, WebKitLoadEvent, const gchar* uri, GError* error, gpointer) {
    std::cerr << "[native-shell load failed] " << uri << ": " << (error ? error->message : "unknown") << "\n";
    return FALSE;
}

int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    const char* url = argc > 1 ? argv[1] : "http://127.0.0.1:8787/ui/?native-shell=1";
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Roon Display");
    gtk_window_set_default_size(GTK_WINDOW(window), 1440, 900);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkWidget* view = webkit_web_view_new();
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), nullptr);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), nullptr);
    gtk_container_add(GTK_CONTAINER(window), view);
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), url);
    gtk_widget_show_all(window);
    gtk_main();
    return EXIT_SUCCESS;
}
