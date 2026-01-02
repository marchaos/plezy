#include "my_application.h"

#include <flutter_linux/flutter_linux.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "flutter/generated_plugin_registrant.h"
#include "mpv/mpv_plugin.h"

struct _MyApplication {
  GtkApplication parent_instance;
  char** dart_entrypoint_arguments;

  // MPV-related widgets
  GtkOverlay* overlay;
  GtkGLArea* gl_area;
  FlView* flutter_view;

  // Method channel for URI handling
  FlMethodChannel* uri_channel;
};

G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION)

/// Sets up an RGBA visual for transparency support.
static void setup_rgba_visual(GtkWidget* widget) {
  GdkScreen* screen = gtk_widget_get_screen(widget);
  if (!gdk_screen_is_composited(screen)) {
    g_warning("Screen is not composited - transparency may not work");
  }
  GdkVisual* visual = gdk_screen_get_rgba_visual(screen);
  if (visual != nullptr) {
    gtk_widget_set_visual(widget, visual);
  } else {
    g_warning("No RGBA visual available");
  }
}

// Implements GApplication::activate.
static void my_application_activate(GApplication* application) {
  MyApplication* self = MY_APPLICATION(application);
  GtkWindow* window =
      GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application)));

  // Use a header bar when running in GNOME as this is the common style used
  // by applications and is the setup most users will be using (e.g. Ubuntu
  // desktop).
  // If running on X and not using GNOME then just use a traditional title bar
  // in case the window manager does more exotic layout, e.g. tiling.
  // If running on Wayland assume the header bar will work (may need changing
  // if future cases occur).
  gboolean use_header_bar = TRUE;
#ifdef GDK_WINDOWING_X11
  GdkScreen* screen = gtk_window_get_screen(window);
  if (GDK_IS_X11_SCREEN(screen)) {
    const gchar* wm_name = gdk_x11_screen_get_window_manager_name(screen);
    if (g_strcmp0(wm_name, "GNOME Shell") != 0) {
      use_header_bar = FALSE;
    }
  }
#endif
  if (use_header_bar) {
    GtkHeaderBar* header_bar = GTK_HEADER_BAR(gtk_header_bar_new());
    gtk_widget_show(GTK_WIDGET(header_bar));
    gtk_header_bar_set_title(header_bar, "Plezy");
    gtk_header_bar_set_show_close_button(header_bar, TRUE);
    gtk_window_set_titlebar(window, GTK_WIDGET(header_bar));
  } else {
    gtk_window_set_title(window, "Plezy");
  }

  gtk_window_set_default_size(window, 1280, 720);

  // Set up RGBA visual for transparency support.
  gtk_widget_set_app_paintable(GTK_WIDGET(window), TRUE);
  setup_rgba_visual(GTK_WIDGET(window));

  // Create the overlay container.
  // The overlay allows us to layer widgets on top of each other:
  // - Bottom layer: GtkGLArea for mpv video rendering
  // - Top layer: FlView (Flutter) with transparent background
  self->overlay = GTK_OVERLAY(gtk_overlay_new());
  gtk_widget_show(GTK_WIDGET(self->overlay));

  // Create the GtkGLArea for mpv video rendering.
  // This will be the bottom layer (behind Flutter).
  self->gl_area = GTK_GL_AREA(gtk_gl_area_new());
  gtk_widget_set_hexpand(GTK_WIDGET(self->gl_area), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(self->gl_area), TRUE);

  // Configure GL area for transparency and proper rendering.
  gtk_gl_area_set_has_alpha(self->gl_area, TRUE);
  gtk_gl_area_set_has_depth_buffer(self->gl_area, FALSE);
  gtk_gl_area_set_has_stencil_buffer(self->gl_area, FALSE);

  // Make GL area non-interactive so mouse events pass through to Flutter.
  gtk_widget_set_can_focus(GTK_WIDGET(self->gl_area), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(self->gl_area), FALSE);

  // Set the GL area as the base widget of the overlay.
  // Initially hidden - will be shown when video playback starts.
  gtk_widget_set_visible(GTK_WIDGET(self->gl_area), FALSE);
  gtk_container_add(GTK_CONTAINER(self->overlay), GTK_WIDGET(self->gl_area));

  // Create the Flutter view.
  g_autoptr(FlDartProject) project = fl_dart_project_new();
  fl_dart_project_set_dart_entrypoint_arguments(project,
                                                self->dart_entrypoint_arguments);

  self->flutter_view = fl_view_new(project);
  gtk_widget_set_hexpand(GTK_WIDGET(self->flutter_view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(self->flutter_view), TRUE);

  // Enable transparency for the Flutter view.
  gtk_widget_set_app_paintable(GTK_WIDGET(self->flutter_view), TRUE);
  setup_rgba_visual(GTK_WIDGET(self->flutter_view));

  // Enable transparent background for the Flutter view.
  // This allows the mpv video to show through transparent areas.
  GdkRGBA transparent = {0.0, 0.0, 0.0, 0.0};
  fl_view_set_background_color(self->flutter_view, &transparent);

  // Add the Flutter view as an overlay on top of the GL area.
  gtk_widget_show(GTK_WIDGET(self->flutter_view));
  gtk_overlay_add_overlay(self->overlay, GTK_WIDGET(self->flutter_view));

  // Add the overlay to the window.
  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(self->overlay));

  // Register Flutter plugins.
  fl_register_plugins(FL_PLUGIN_REGISTRY(self->flutter_view));

  // Register the MPV plugin with the GL area and Flutter view for video rendering.
  FlPluginRegistrar* registrar =
      fl_plugin_registry_get_registrar_for_plugin(FL_PLUGIN_REGISTRY(self->flutter_view),
                                                  "MpvPlugin");
  mpv_plugin_register_with_registrar(registrar, self->overlay, self->gl_area,
                                     GTK_WIDGET(self->flutter_view));

  gtk_widget_show(GTK_WIDGET(window));
  gtk_widget_grab_focus(GTK_WIDGET(self->flutter_view));

  // Set up method channel for URI handling
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  self->uri_channel = fl_method_channel_new(
      fl_engine_get_binary_messenger(fl_view_get_engine(self->flutter_view)),
      "com.edde746.plezy/uri",
      FL_METHOD_CODEC(codec));
}

// Send URI to Flutter via method channel
static void send_uri_to_flutter(MyApplication* self, const gchar* uri) {
  if (self->uri_channel == nullptr) {
    g_warning("URI channel not initialized, cannot send URI: %s", uri);
    return;
  }

  g_autoptr(FlValue) args = fl_value_new_string(uri);
  fl_method_channel_invoke_method(self->uri_channel, "onUri", args,
                                  nullptr, nullptr, nullptr);
}

// Implements GApplication::open - handles URIs from other instances
static void my_application_open(GApplication* application,
                                GFile** files,
                                gint n_files,
                                const gchar* hint) {
  MyApplication* self = MY_APPLICATION(application);

  // Bring window to front
  GList* windows = gtk_application_get_windows(GTK_APPLICATION(application));
  if (windows != nullptr) {
    gtk_window_present(GTK_WINDOW(windows->data));
  }

  // Forward URIs to Flutter
  for (gint i = 0; i < n_files; i++) {
    g_autofree gchar* uri = g_file_get_uri(files[i]);
    if (uri != nullptr) {
      send_uri_to_flutter(self, uri);
    }
  }
}

// Implements GApplication::local_command_line.
static gboolean my_application_local_command_line(GApplication* application,
                                                  gchar*** arguments,
                                                  int* exit_status) {
  MyApplication* self = MY_APPLICATION(application);
  // Strip out the first argument as it is the binary name.
  self->dart_entrypoint_arguments = g_strdupv(*arguments + 1);

  g_autoptr(GError) error = nullptr;
  if (!g_application_register(application, nullptr, &error)) {
    g_warning("Failed to register: %s", error->message);
    *exit_status = 1;
    return TRUE;
  }

  // Check if this is a URI invocation
  gchar** args = *arguments;
  gint argc = g_strv_length(args);
  gboolean has_uri = FALSE;

  for (gint i = 1; i < argc; i++) {
    if (g_str_has_prefix(args[i], "plezy://")) {
      has_uri = TRUE;
      break;
    }
  }

  if (has_uri && g_application_get_is_remote(application)) {
    // Another instance is running - send URIs to it
    g_autoptr(GFile)* files = g_new0(GFile*, argc);
    gint n_files = 0;

    for (gint i = 1; i < argc; i++) {
      if (g_str_has_prefix(args[i], "plezy://")) {
        files[n_files++] = g_file_new_for_uri(args[i]);
      }
    }

    if (n_files > 0) {
      g_application_open(application, files, n_files, "");
    }

    for (gint i = 0; i < n_files; i++) {
      g_object_unref(files[i]);
    }
    g_free(files);

    *exit_status = 0;
    return TRUE;
  }

  g_application_activate(application);
  *exit_status = 0;

  return TRUE;
}

// Implements GApplication::startup.
static void my_application_startup(GApplication* application) {
  // MyApplication* self = MY_APPLICATION(object);

  // Perform any actions required at application startup.

  G_APPLICATION_CLASS(my_application_parent_class)->startup(application);
}

// Implements GApplication::shutdown.
static void my_application_shutdown(GApplication* application) {
  // MyApplication* self = MY_APPLICATION(object);

  // Perform any actions required at application shutdown.

  G_APPLICATION_CLASS(my_application_parent_class)->shutdown(application);
}

// Implements GObject::dispose.
static void my_application_dispose(GObject* object) {
  MyApplication* self = MY_APPLICATION(object);
  g_clear_pointer(&self->dart_entrypoint_arguments, g_strfreev);
  G_OBJECT_CLASS(my_application_parent_class)->dispose(object);
}

static void my_application_class_init(MyApplicationClass* klass) {
  G_APPLICATION_CLASS(klass)->activate = my_application_activate;
  G_APPLICATION_CLASS(klass)->open = my_application_open;
  G_APPLICATION_CLASS(klass)->local_command_line =
      my_application_local_command_line;
  G_APPLICATION_CLASS(klass)->startup = my_application_startup;
  G_APPLICATION_CLASS(klass)->shutdown = my_application_shutdown;
  G_OBJECT_CLASS(klass)->dispose = my_application_dispose;
}

static void my_application_init(MyApplication* self) {
  self->overlay = nullptr;
  self->gl_area = nullptr;
  self->flutter_view = nullptr;
  self->uri_channel = nullptr;
}

MyApplication* my_application_new() {
  // Set the program name to the application ID, which helps various systems
  // like GTK and desktop environments map this running application to its
  // corresponding .desktop file. This ensures better integration by allowing
  // the application to be recognized beyond its binary name.
  g_set_prgname(APPLICATION_ID);

  return MY_APPLICATION(g_object_new(my_application_get_type(),
                                     "application-id", APPLICATION_ID,
                                     "flags", G_APPLICATION_HANDLES_OPEN,
                                     nullptr));
}
