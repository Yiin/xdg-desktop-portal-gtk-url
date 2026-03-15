/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gtk/gtk.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include <glib/gi18n.h>

#include "xdg-desktop-portal-dbus.h"

#include "filechooser.h"
#include "request.h"
#include "utils.h"
#include "gtkbackports.h"
#include "externalwindow.h"


typedef struct {
  XdpImplFileChooser *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GtkWidget *dialog;
  GtkFileChooserAction action;
  gboolean multiple;
  ExternalWindow *external_parent;

  GSList *files;

  GtkFileFilter *filter;

  int response;
  GSList *uris;

  gboolean allow_write;

  GHashTable *choices;

  /* Filename bar + URL download support */
  GtkWidget *url_entry;
  GtkWidget *url_spinner;
  GtkWidget *url_status;
  GtkWidget *filter_combo;
  GtkWidget *filter_label;
  GCancellable *url_cancellable;
  char *download_path;
} FileDialogHandle;

typedef struct {
  FileDialogHandle *handle;
  GCancellable *cancellable;
} UrlAsyncData;

static void
file_dialog_handle_free (gpointer data)
{
  FileDialogHandle *handle = data;

  g_clear_object (&handle->external_parent);
  if (handle->url_cancellable)
    {
      g_cancellable_cancel (handle->url_cancellable);
      g_object_unref (handle->url_cancellable);
    }
  g_free (handle->download_path);
  g_object_unref (handle->dialog);
  g_object_unref (handle->request);
  g_slist_free_full (handle->files, g_free);
  g_slist_free_full (handle->uris, g_free);
  g_hash_table_unref (handle->choices);

  g_free (handle);
}

static void
file_dialog_handle_close (FileDialogHandle *handle)
{
  gtk_widget_destroy (handle->dialog);
  file_dialog_handle_free (handle);
}

static void
add_choices (FileDialogHandle *handle,
             GVariantBuilder *builder)
{
  GVariantBuilder choices;
  GHashTableIter iter;
  const char *id;
  const char *selected;

  g_variant_builder_init (&choices, G_VARIANT_TYPE ("a(ss)"));
  g_hash_table_iter_init (&iter, handle->choices);
  while (g_hash_table_iter_next (&iter, (gpointer *)&id, (gpointer *)&selected))
    g_variant_builder_add (&choices, "(ss)", id, selected);

  g_variant_builder_add (builder, "{sv}", "choices", g_variant_builder_end (&choices));
}

static void
add_recent_entry (const char *app_id,
                  const char *uri)
{
  GtkRecentManager *recent;
  GtkRecentData data;

  /* These fields are ignored by everybody, so it is not worth
   * spending effort on filling them out. Just use defaults.
   */
  data.display_name = NULL;
  data.description = NULL;
  data.mime_type = "application/octet-stream";
  data.app_name = (char *)app_id;
  data.app_exec = "gio open %u";
  data.groups = NULL;
  data.is_private = FALSE;

  recent = gtk_recent_manager_get_default ();
  gtk_recent_manager_add_full (recent, uri, &data);
}

static void
send_response (FileDialogHandle *handle)
{
  GVariantBuilder uri_builder;
  GVariantBuilder opt_builder;
  GSList *l;
  const char *method_name;

  method_name = g_dbus_method_invocation_get_method_name (handle->invocation);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (strcmp (method_name, "SaveFiles") == 0 &&
      handle->action == GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER &&
      handle->uris)
    {
      g_autoptr(GFile) base_dir = g_file_new_for_uri (handle->uris->data);

      g_slist_free_full (handle->uris, g_free);
      handle->uris = NULL;

      for (l = handle->files; l; l = l->next)
        {
          int uniqifier = 0;
          const char *file_name = l->data;
          g_autoptr(GFile) file = g_file_get_child (base_dir, file_name);

          while (g_file_query_exists(file, NULL))
            {
              g_autofree char *base_name = g_file_get_basename (file);
              g_auto(GStrv) parts = NULL;
              g_autoptr(GString) unique_name = NULL;

              parts = g_strsplit (base_name, ".", 2);

              unique_name = g_string_new (parts[0]);
              g_string_append_printf (unique_name, "(%i)", ++uniqifier);
              if (parts[1] != NULL)
                  g_string_append (unique_name, parts[1]);

              file = g_file_get_child (base_dir, unique_name->str);
            }
          handle->uris = g_slist_append (handle->uris, g_file_get_uri (file));
        }
    }

  g_variant_builder_init (&uri_builder, G_VARIANT_TYPE_STRING_ARRAY);
  for (l = handle->uris; l; l = l->next)
    {
      add_recent_entry (handle->request->app_id, l->data);
      g_variant_builder_add (&uri_builder, "s", l->data);
    }

  if (handle->filter) {
    GVariant *current_filter_variant = gtk_file_filter_to_gvariant (handle->filter);
    g_variant_builder_add (&opt_builder, "{sv}", "current_filter", current_filter_variant);
  }

  g_variant_builder_add (&opt_builder, "{sv}", "uris", g_variant_builder_end (&uri_builder));
  g_variant_builder_add (&opt_builder, "{sv}", "writable", g_variant_new_boolean (handle->allow_write));

  add_choices (handle, &opt_builder);

  if (handle->request->exported)
    request_unexport (handle->request);

  if (strcmp (method_name, "OpenFile") == 0)
    xdp_impl_file_chooser_complete_open_file (handle->impl,
                                              handle->invocation,
                                              handle->response,
                                              g_variant_builder_end (&opt_builder));
  else if (strcmp (method_name, "SaveFile") == 0)
    xdp_impl_file_chooser_complete_save_file (handle->impl,
                                              handle->invocation,
                                              handle->response,
                                              g_variant_builder_end (&opt_builder));
  else if (strcmp (method_name, "SaveFiles") == 0)
    xdp_impl_file_chooser_complete_save_files (handle->impl,
                                               handle->invocation,
                                               handle->response,
                                               g_variant_builder_end (&opt_builder));
  else
    g_assert_not_reached ();

  file_dialog_handle_close (handle);
}

static void start_url_download (FileDialogHandle *handle, const char *url);

static void
file_chooser_response (GtkWidget *widget,
                       int response,
                       gpointer user_data)
{
  FileDialogHandle *handle = user_data;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      handle->response = 2;
      handle->filter = NULL;
      handle->uris = NULL;
      break;

    case GTK_RESPONSE_DELETE_EVENT:
    case GTK_RESPONSE_CANCEL:
      handle->response = 1;
      handle->filter = NULL;
      handle->uris = NULL;
      break;

    case GTK_RESPONSE_OK:
      if (handle->url_entry)
        {
          const char *text = gtk_entry_get_text (GTK_ENTRY (handle->url_entry));

          /* URL — download in background, send response on completion */
          if (text && (g_str_has_prefix (text, "http://") ||
                       g_str_has_prefix (text, "https://")))
            {
              start_url_download (handle, text);
              return;
            }

          /* Resolve typed path */
          if (text && text[0])
            {
              g_autofree char *resolved = NULL;

              if (text[0] == '/')
                resolved = g_strdup (text);
              else
                {
                  g_autofree char *folder = gtk_file_chooser_get_current_folder (
                      GTK_FILE_CHOOSER (widget));
                  if (folder)
                    resolved = g_build_filename (folder, text, NULL);
                }

              if (resolved)
                {
                  if (g_file_test (resolved, G_FILE_TEST_IS_DIR))
                    {
                      gtk_file_chooser_set_current_folder (
                          GTK_FILE_CHOOSER (widget), resolved);
                      gtk_entry_set_text (GTK_ENTRY (handle->url_entry), "");
                      return; /* Navigate, don't close */
                    }
                  if (g_file_test (resolved, G_FILE_TEST_EXISTS))
                    gtk_file_chooser_set_filename (
                        GTK_FILE_CHOOSER (widget), resolved);
                }
            }
        }

      handle->response = 0;
      handle->filter = gtk_file_chooser_get_filter (GTK_FILE_CHOOSER(widget));
      handle->uris = gtk_file_chooser_get_uris (GTK_FILE_CHOOSER (widget));
      break;
    }

  send_response (handle);
}

static void
read_only_toggled (GtkToggleButton *button, gpointer user_data)
{
  FileDialogHandle *handle = user_data;

  handle->allow_write = !gtk_toggle_button_get_active (button);
}

static void
choice_changed (GtkComboBox *combo,
                FileDialogHandle *handle)
{
  const char *id;
  const char *selected;

  id = (const char *)g_object_get_data (G_OBJECT (combo), "choice-id");
  selected = gtk_combo_box_get_active_id (combo);
  g_hash_table_replace (handle->choices, (gpointer)id, (gpointer)selected);
}

static void
choice_toggled (GtkToggleButton *toggle,
                FileDialogHandle *handle)
{
  const char *id;
  const char *selected;

  id = (const char *)g_object_get_data (G_OBJECT (toggle), "choice-id");
  selected = gtk_toggle_button_get_active (toggle) ? "true" : "false";
  g_hash_table_replace (handle->choices, (gpointer)id, (gpointer)selected);
}

static GtkWidget *
deserialize_choice (GVariant *choice,
                    FileDialogHandle *handle)
{
  GtkWidget *widget;
  const char *choice_id;
  const char *label;
  const char *selected;
  GVariant *choices;
  int i;

  g_variant_get (choice, "(&s&s@a(ss)&s)", &choice_id, &label, &choices, &selected);

  if (g_variant_n_children (choices) > 0)
    {
      GtkWidget *box;
      GtkWidget *combo;

      box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_container_add (GTK_CONTAINER (box), gtk_label_new (label));

      combo = gtk_combo_box_text_new ();
      g_object_set_data_full (G_OBJECT (combo), "choice-id", g_strdup (choice_id), g_free);
      gtk_container_add (GTK_CONTAINER (box), combo);

      for (i = 0; i < g_variant_n_children (choices); i++)
        {
          const char *id;
          const char *text;

          g_variant_get_child (choices, i, "(&s&s)", &id, &text);
          gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), id, text);
        }

      if (strcmp (selected, "") == 0)
        g_variant_get_child (choices, 0, "(&s&s)", &selected, NULL);

      g_signal_connect (combo, "changed", G_CALLBACK (choice_changed), handle);
      gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), selected);

      widget = box;
    }
  else
    {
      GtkWidget *check;

      check = gtk_check_button_new_with_label (label);
      g_object_set_data_full (G_OBJECT (check), "choice-id", g_strdup (choice_id), g_free);
      g_signal_connect (check, "toggled", G_CALLBACK (choice_toggled), handle);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), g_strcmp0 (selected, "true") == 0);

      widget = check;
    }

  gtk_widget_show_all (widget);

  return widget;
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              FileDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->action == GTK_FILE_CHOOSER_ACTION_OPEN)
    xdp_impl_file_chooser_complete_open_file (handle->impl,
                                              handle->invocation,
                                              2,
                                              g_variant_builder_end (&opt_builder));
  else if (handle->action == GTK_FILE_CHOOSER_ACTION_SAVE)
    xdp_impl_file_chooser_complete_save_file (handle->impl,
                                              handle->invocation,
                                              2,
                                              g_variant_builder_end (&opt_builder));
  else
    xdp_impl_file_chooser_complete_save_files (handle->impl,
                                               handle->invocation,
                                               2,
                                               g_variant_builder_end (&opt_builder));

  if (handle->request->exported)
    request_unexport (handle->request);

  file_dialog_handle_close (handle);

  xdp_impl_request_complete_close (object, invocation);

  return TRUE;
}

static void
update_preview_cb (GtkFileChooser *file_chooser, gpointer data)
{
  GtkWidget *preview = GTK_WIDGET (data);
  g_autofree char *filename = NULL;
  g_autoptr(GdkPixbuf) pixbuf = NULL;

  filename = gtk_file_chooser_get_preview_filename (file_chooser);
  if (filename)
    pixbuf = gdk_pixbuf_new_from_file_at_size (filename, 128, 128, NULL);

  if (pixbuf)
    {
      g_autoptr(GdkPixbuf) tmp = NULL;

      tmp = gdk_pixbuf_apply_embedded_orientation (pixbuf);
      g_set_object (&pixbuf, tmp);
    }

  gtk_image_set_from_pixbuf (GTK_IMAGE (preview), pixbuf);
  gtk_file_chooser_set_preview_widget_active (file_chooser, pixbuf != NULL);
}

/* Simplify case-insensitive globs like *.[jJ][pP][gG] → *.jpg */
static char *
simplify_glob_pattern (const char *pattern)
{
  GString *out = g_string_new (NULL);
  const char *p = pattern;

  while (*p)
    {
      if (p[0] == '[' && p[1] && p[2] && p[3] == ']' &&
          p[1] != p[2] &&
          g_ascii_tolower (p[1]) == g_ascii_tolower (p[2]))
        {
          g_string_append_c (out, g_ascii_tolower (p[1]));
          p += 4;
        }
      else
        {
          g_string_append_c (out, *p);
          p++;
        }
    }

  return g_string_free (out, FALSE);
}

/* URL download support */

static char *
url_extract_filename (const char *url)
{
  const char *p, *end, *last_slash;

  p = strstr (url, "://");
  p = p ? p + 3 : url;

  end = strpbrk (p, "?#");
  if (!end)
    end = p + strlen (p);

  last_slash = NULL;
  for (const char *c = p; c < end; c++)
    if (*c == '/')
      last_slash = c;

  if (last_slash && last_slash + 1 < end)
    {
      g_autofree char *encoded = g_strndup (last_slash + 1, end - (last_slash + 1));
      char *decoded = g_uri_unescape_string (encoded, NULL);
      if (decoded && decoded[0])
        return decoded;
      g_free (decoded);
    }

  return g_strdup ("download");
}

static char *
make_unique_download_path (const char *dir, const char *filename)
{
  g_autofree char *path = g_build_filename (dir, filename, NULL);
  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return g_steal_pointer (&path);

  const char *dot = strrchr (filename, '.');
  g_autofree char *base = NULL;
  const char *ext = "";

  if (dot && dot != filename)
    {
      base = g_strndup (filename, dot - filename);
      ext = dot;
    }
  else
    {
      base = g_strdup (filename);
    }

  for (int i = 1; i < 1000; i++)
    {
      g_autofree char *name = g_strdup_printf ("%s(%d)%s", base, i, ext);
      g_autofree char *p = g_build_filename (dir, name, NULL);
      if (!g_file_test (p, G_FILE_TEST_EXISTS))
        return g_steal_pointer (&p);
    }

  return g_build_filename (dir, filename, NULL);
}

static void
on_url_download_finished (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  UrlAsyncData *data = user_data;
  GSubprocess *proc = G_SUBPROCESS (source);
  g_autoptr(GError) error = NULL;

  g_subprocess_wait_finish (proc, result, &error);

  if (g_cancellable_is_cancelled (data->cancellable))
    {
      g_object_unref (data->cancellable);
      g_free (data);
      return;
    }

  FileDialogHandle *handle = data->handle;
  g_object_unref (data->cancellable);
  g_free (data);

  gtk_spinner_stop (GTK_SPINNER (handle->url_spinner));
  gtk_widget_hide (handle->url_spinner);

  if (error || !g_subprocess_get_successful (proc))
    {
      /* Download failed — re-enable dialog so user can retry */
      gtk_widget_set_sensitive (handle->url_entry, TRUE);
      gtk_dialog_set_response_sensitive (GTK_DIALOG (handle->dialog),
                                         GTK_RESPONSE_OK, TRUE);
      gtk_label_set_markup (GTK_LABEL (handle->url_status),
                            "<span color=\"red\">Download failed</span>");
      if (handle->download_path)
        {
          unlink (handle->download_path);
          g_clear_pointer (&handle->download_path, g_free);
        }
      return;
    }

  /* Download succeeded — complete the Open operation */
  handle->response = 0;
  handle->filter = gtk_file_chooser_get_filter (GTK_FILE_CHOOSER (handle->dialog));
  handle->uris = g_slist_append (NULL,
      g_filename_to_uri (handle->download_path, NULL, NULL));
  g_clear_pointer (&handle->download_path, g_free);

  send_response (handle);
}

static void
start_url_download (FileDialogHandle *handle, const char *url)
{
  g_autofree char *filename = NULL;
  g_autofree char *tmp_dir = NULL;
  g_autoptr(GSubprocess) proc = NULL;
  g_autoptr(GError) error = NULL;
  UrlAsyncData *async_data;

  tmp_dir = g_build_filename (g_get_tmp_dir (), "portal-url-downloads", NULL);
  g_mkdir_with_parents (tmp_dir, 0700);

  filename = url_extract_filename (url);
  g_free (handle->download_path);
  handle->download_path = make_unique_download_path (tmp_dir, filename);

  /* Disable dialog while downloading */
  gtk_label_set_text (GTK_LABEL (handle->url_status), "Downloading\u2026");
  gtk_widget_show (handle->url_spinner);
  gtk_spinner_start (GTK_SPINNER (handle->url_spinner));
  gtk_widget_set_sensitive (handle->url_entry, FALSE);
  gtk_dialog_set_response_sensitive (GTK_DIALOG (handle->dialog),
                                     GTK_RESPONSE_OK, FALSE);

  proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                           &error,
                           "curl", "-fSL",
                           "--max-time", "120",
                           "--max-filesize", "1073741824",
                           "-o", handle->download_path,
                           url,
                           NULL);

  if (!proc)
    {
      gtk_spinner_stop (GTK_SPINNER (handle->url_spinner));
      gtk_widget_hide (handle->url_spinner);
      gtk_widget_set_sensitive (handle->url_entry, TRUE);
      gtk_dialog_set_response_sensitive (GTK_DIALOG (handle->dialog),
                                         GTK_RESPONSE_OK, TRUE);
      gtk_label_set_markup (GTK_LABEL (handle->url_status),
                            "<span color=\"red\">Failed to start download</span>");
      return;
    }

  async_data = g_new0 (UrlAsyncData, 1);
  async_data->handle = handle;
  async_data->cancellable = g_object_ref (handle->url_cancellable);

  g_subprocess_wait_async (proc, handle->url_cancellable,
                           on_url_download_finished, async_data);
}

static void
on_selection_changed (GtkFileChooser *chooser, gpointer user_data)
{
  FileDialogHandle *handle = user_data;
  g_autofree char *filename = NULL;

  if (!handle->url_entry)
    return;

  /* Don't overwrite during active download */
  if (!gtk_widget_get_sensitive (handle->url_entry))
    return;

  filename = gtk_file_chooser_get_filename (chooser);
  if (filename)
    {
      g_autofree char *basename = g_path_get_basename (filename);
      gtk_entry_set_text (GTK_ENTRY (handle->url_entry), basename);
      gtk_label_set_text (GTK_LABEL (handle->url_status), "");
    }
}

static void
on_filter_menu_item_toggled (GtkCheckMenuItem *item, gpointer user_data)
{
  FileDialogHandle *handle = user_data;

  if (!gtk_check_menu_item_get_active (item))
    return;

  /* Apply the filter */
  GtkFileFilter *filter = g_object_get_data (G_OBJECT (item), "filter");
  if (filter)
    gtk_file_chooser_set_filter (GTK_FILE_CHOOSER (handle->dialog), filter);

  /* Update button label and tooltip */
  const char *full_text = g_object_get_data (G_OBJECT (item), "full-text");
  if (full_text && handle->filter_label)
    {
      gtk_label_set_text (GTK_LABEL (handle->filter_label), full_text);
      gtk_widget_set_tooltip_text (handle->filter_combo, full_text);
    }
}

static void
on_filename_entry_activated (GtkEntry *entry, gpointer user_data)
{
  FileDialogHandle *handle = user_data;

  /* Enter triggers OK — all logic is in the response handler */
  gtk_dialog_response (GTK_DIALOG (handle->dialog), GTK_RESPONSE_OK);
}

static void
hide_builtin_filter_cb (GtkWidget *widget, gpointer user_data)
{
  /* Hide GTK's built-in filter combo */
  if (GTK_IS_COMBO_BOX (widget))
    gtk_widget_hide (widget);
  else if (GTK_IS_CONTAINER (widget))
    gtk_container_foreach (GTK_CONTAINER (widget),
                           hide_builtin_filter_cb, NULL);
}

static GtkWidget *
create_filename_bar (FileDialogHandle *handle)
{
  GtkWidget *hbox, *label, *entry, *spinner, *status;

  hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  g_object_set (hbox, "margin-start", 10, "margin-end", 10,
                "margin-top", 4, "margin-bottom", 4, NULL);

  label = gtk_label_new ("File name:");
  gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

  entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (entry),
                                  "Select a file or paste a URL\u2026");
  gtk_widget_set_hexpand (entry, TRUE);
  g_signal_connect (entry, "activate",
                    G_CALLBACK (on_filename_entry_activated), handle);
  gtk_box_pack_start (GTK_BOX (hbox), entry, TRUE, TRUE, 0);
  handle->url_entry = entry;

  spinner = gtk_spinner_new ();
  gtk_widget_set_no_show_all (spinner, TRUE);
  gtk_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);
  handle->url_spinner = spinner;

  /* Build filter selector as MenuButton + Menu for independent styling */
  {
    GSList *filters = gtk_file_chooser_list_filters (
        GTK_FILE_CHOOSER (handle->dialog));
    if (filters)
      {
        GtkWidget *filter_button = gtk_menu_button_new ();
        GtkWidget *filter_label = gtk_label_new ("");
        GtkWidget *filter_menu = gtk_menu_new ();
        GSList *l;
        GSList *group = NULL;
        const char *first_text = NULL;

        gtk_label_set_ellipsize (GTK_LABEL (filter_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars (GTK_LABEL (filter_label), 30);
        /* Replace the button's default child with our label */
        gtk_container_add (GTK_CONTAINER (filter_button), filter_label);

        handle->filter_combo = filter_button;
        handle->filter_label = filter_label;

        GtkFileFilter *current = gtk_file_chooser_get_filter (
            GTK_FILE_CHOOSER (handle->dialog));
        int current_idx = current ? g_slist_index (filters, current) : 0;
        if (current_idx < 0) current_idx = 0;

        int i = 0;
        for (l = filters; l; l = l->next, i++)
          {
            GtkFileFilter *f = l->data;
            g_autoptr(GVariant) fv = gtk_file_filter_to_gvariant (f);
            const char *name;
            g_autoptr(GVariant) rules = NULL;
            g_autoptr(GString) desc = g_string_new (NULL);

            g_variant_get (fv, "(&s@a(us))", &name, &rules);

            for (int j = 0; j < (int) g_variant_n_children (rules); j++)
              {
                guint32 type;
                const char *pattern;
                g_variant_get_child (rules, j, "(u&s)", &type, &pattern);
                if (desc->len > 0)
                  g_string_append (desc, "; ");
                if (type == 0)
                  {
                    g_autofree char *simple = simplify_glob_pattern (pattern);
                    g_string_append (desc, simple);
                  }
                else
                  g_string_append (desc, pattern);
              }

            g_autofree char *full = NULL;
            if (desc->len > 0)
              full = g_strdup_printf ("%s (%s)", name, desc->str);
            else
              full = g_strdup (name);

            GtkWidget *item = gtk_radio_menu_item_new_with_label (group, full);
            group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));

            /* Wrap long labels in menu items */
            GtkWidget *item_label = gtk_bin_get_child (GTK_BIN (item));
            if (GTK_IS_LABEL (item_label))
              {
                gtk_label_set_line_wrap (GTK_LABEL (item_label), TRUE);
                gtk_label_set_max_width_chars (GTK_LABEL (item_label), 60);
              }

            g_object_set_data (G_OBJECT (item), "filter", f);
            g_object_set_data_full (G_OBJECT (item), "full-text",
                                    g_strdup (full), g_free);
            g_signal_connect (item, "toggled",
                              G_CALLBACK (on_filter_menu_item_toggled), handle);
            gtk_menu_shell_append (GTK_MENU_SHELL (filter_menu), item);

            if (i == current_idx)
              {
                gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), TRUE);
                first_text = g_object_get_data (G_OBJECT (item), "full-text");
              }
          }

        gtk_widget_show_all (filter_menu);
        gtk_menu_button_set_popup (GTK_MENU_BUTTON (filter_button), filter_menu);

        /* Set initial label and tooltip */
        if (first_text)
          {
            gtk_label_set_text (GTK_LABEL (filter_label), first_text);
            gtk_widget_set_tooltip_text (filter_button, first_text);
          }

        gtk_box_pack_start (GTK_BOX (hbox), filter_button, FALSE, FALSE, 0);
        g_slist_free (filters);
      }
  }

  status = gtk_label_new ("");
  gtk_box_pack_start (GTK_BOX (hbox), status, FALSE, FALSE, 0);
  handle->url_status = status;

  gtk_widget_show_all (hbox);
  return hbox;
}

static gboolean
handle_open (XdpImplFileChooser *object,
             GDBusMethodInvocation *invocation,
             const char *arg_handle,
             const char *arg_app_id,
             const char *arg_parent_window,
             const char *arg_title,
             GVariant *arg_options)
{
  g_autoptr(Request) request = NULL;
  const gchar *method_name;
  const gchar *sender;
  GtkFileChooserAction action;
  gboolean multiple;
  gboolean directory;
  gboolean modal;
  GdkDisplay *display;
  GdkScreen *screen;
  GtkWidget *dialog;
  ExternalWindow *external_parent = NULL;
  GtkWidget *fake_parent;
  FileDialogHandle *handle;
  const char *cancel_label;
  const char *accept_label;
  GVariantIter *iter;
  const char *current_name;
  const char *path;
  g_autoptr (GVariant) choices = NULL;
  g_autoptr (GVariant) current_filter = NULL;
  GSList *filters = NULL;
  GtkWidget *preview;

  method_name = g_dbus_method_invocation_get_method_name (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  request = request_new (sender, arg_app_id, arg_handle);

  if (strcmp (method_name, "SaveFile") == 0)
    {
      action = GTK_FILE_CHOOSER_ACTION_SAVE;
      multiple = FALSE;
    }
  else if (strcmp (method_name, "SaveFiles") == 0)
    {
      action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
      multiple = FALSE;
    }
  else
    {
      if (!g_variant_lookup (arg_options, "multiple", "b", &multiple))
        multiple = FALSE;
      if (!g_variant_lookup (arg_options, "directory", "b", &directory))
        directory = FALSE;
      action = directory ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN;
    }

  if (!g_variant_lookup (arg_options, "modal", "b", &modal))
    modal = TRUE;

  if (!g_variant_lookup (arg_options, "accept_label", "&s", &accept_label))
    {
      if (strcmp (method_name, "OpenFile") == 0)
        accept_label = multiple ? _("_Open") : _("_Select");
      else
        accept_label = _("_Save");
    }

  cancel_label = _("_Cancel");

  if (arg_parent_window)
    {
      external_parent = create_external_window_from_handle (arg_parent_window);
      if (!external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   arg_parent_window);
    }

  if (external_parent)
    display = external_window_get_display (external_parent);
  else
    display = gdk_display_get_default ();
  screen = gdk_display_get_default_screen (display);

  fake_parent = g_object_new (GTK_TYPE_WINDOW,
                              "type", GTK_WINDOW_TOPLEVEL,
                              "screen", screen,
                              NULL);
  g_object_ref_sink (fake_parent);

  dialog = gtk_file_chooser_dialog_new (arg_title, GTK_WINDOW (fake_parent), action,
                                        cancel_label, GTK_RESPONSE_CANCEL,
                                        accept_label, GTK_RESPONSE_OK,
                                        NULL);
  gtk_window_set_modal (GTK_WINDOW (dialog), modal);

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
  gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER (dialog), multiple);

  preview = gtk_image_new ();
  g_object_set (preview, "margin", 10, NULL);
  gtk_widget_show (preview);
  gtk_file_chooser_set_preview_widget (GTK_FILE_CHOOSER (dialog), preview);
  gtk_file_chooser_set_preview_widget_active (GTK_FILE_CHOOSER (dialog), FALSE);
  gtk_file_chooser_set_use_preview_label (GTK_FILE_CHOOSER (dialog), FALSE);
  g_signal_connect (dialog, "update-preview", G_CALLBACK (update_preview_cb), preview);

  handle = g_new0 (FileDialogHandle, 1);
  handle->impl = object;
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->dialog = g_object_ref (dialog);
  handle->action = action;
  handle->multiple = multiple;
  handle->choices = g_hash_table_new (g_str_hash, g_str_equal);
  handle->external_parent = external_parent;
  handle->allow_write = TRUE;
  handle->url_cancellable = g_cancellable_new ();

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  g_signal_connect (dialog, "response", G_CALLBACK (file_chooser_response), handle);

  choices = g_variant_lookup_value (arg_options, "choices", G_VARIANT_TYPE ("a(ssa(ss)s)"));
  if (choices)
    {
      int i;
      GtkWidget *box;

      box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_widget_show (box);
      for (i = 0; i < g_variant_n_children (choices); i++)
        {
          GVariant *value = g_variant_get_child_value (choices, i);
          gtk_container_add (GTK_CONTAINER (box), deserialize_choice (value, handle));
        }

      gtk_file_chooser_set_extra_widget (GTK_FILE_CHOOSER (dialog), box);
    }

  if (g_variant_lookup (arg_options, "filters", "a(sa(us))", &iter))
    {
      GVariant *variant;

      while (g_variant_iter_next (iter, "@(sa(us))", &variant))
        {
          GtkFileFilter *filter;

          filter = gtk_file_filter_new_from_gvariant (variant);
          filters = g_slist_append (filters, g_object_ref (filter));
          gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);
          g_variant_unref (variant);
        }
      g_variant_iter_free (iter);
    }

  if (g_variant_lookup (arg_options, "current_filter", "@(sa(us))", &current_filter))
    {
      g_autoptr (GtkFileFilter) filter = NULL;
      const char *current_filter_name;

      filter = g_object_ref_sink (gtk_file_filter_new_from_gvariant (current_filter));
      current_filter_name = gtk_file_filter_get_name (filter);

      if (!filters)
        {
          /* We are setting a single, unchangeable filter. */
          gtk_file_chooser_set_filter (GTK_FILE_CHOOSER (dialog), filter);
        }
      else
        {
          gboolean handled = FALSE;

          /* We are trying to select the default filter from the list of
           * filters. We want to naively take filter and pass it to
           * gtk_file_chooser_set_filter(), but it's not good enough
           * because GTK just compares filters by pointer value, so the
           * pointer itself has to match. We'll use the heuristic that
           * if two filters have the same name, they must be the same
           * unless the application is very dumb.
           */
          for (GSList *l = filters; l; l = l->next)
            {
              GtkFileFilter *f = l->data;
              const char *name = gtk_file_filter_get_name (f);

              if (g_strcmp0 (name, current_filter_name) == 0)
                {
                  gtk_file_chooser_set_filter (GTK_FILE_CHOOSER (dialog), f);
                  handled = TRUE;
                  break;
                }
            }

          if (!handled)
            g_warning ("current file filter must be present in filters list when list is nonempty");
        }
    }
  g_slist_free_full (filters, g_object_unref);

  if (strcmp (method_name, "OpenFile") == 0)
    {
      if (g_variant_lookup (arg_options, "current_folder", "^&ay", &path))
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), path);
    }
  else if (strcmp (method_name, "SaveFile") == 0)
    {
      if (g_variant_lookup (arg_options, "current_name", "&s", &current_name))
        gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), current_name);
      /* TODO: is this useful ?
       * In a sandboxed situation, the current folder and current file
       * are likely in the fuse filesystem
       */
      if (g_variant_lookup (arg_options, "current_folder", "^&ay", &path))
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), path);
      if (g_variant_lookup (arg_options, "current_file", "^&ay", &path))
        gtk_file_chooser_select_filename (GTK_FILE_CHOOSER (dialog), path);
    }
  else if (strcmp (method_name, "SaveFiles") == 0)
    {
      /* TODO: is this useful ?
       * In a sandboxed situation, the current folder and current file
       * are likely in the fuse filesystem
       */
      if (g_variant_lookup (arg_options, "current_folder", "^&ay", &path))
        gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), path);

      if (g_variant_lookup (arg_options, "files", "aay", &iter))
        {
          char *file = NULL;
          while (g_variant_iter_next (iter, "^ay", &file))
            handle->files = g_slist_append (handle->files, file);

          g_variant_iter_free (iter);
        }
    }

  g_object_unref (fake_parent);

  gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER (dialog), TRUE);

  if (action == GTK_FILE_CHOOSER_ACTION_OPEN)
    {
      GtkWidget *readonly;
      GtkWidget *extra;

      extra = gtk_file_chooser_get_extra_widget (GTK_FILE_CHOOSER (dialog));

      readonly = gtk_check_button_new_with_label (_("Open files read-only"));
      gtk_widget_show (readonly);

      g_signal_connect (readonly, "toggled",
                        G_CALLBACK (read_only_toggled), handle);

      if (GTK_IS_CONTAINER (extra))
        gtk_container_add (GTK_CONTAINER (extra), readonly);
      else
        gtk_file_chooser_set_extra_widget (GTK_FILE_CHOOSER (dialog), readonly);
    }

  /* Add Windows-style filename bar for OpenFile action */
  if (action == GTK_FILE_CHOOSER_ACTION_OPEN)
    {
      GtkWidget *filename_bar = create_filename_bar (handle);
      GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
      gtk_box_pack_start (GTK_BOX (content_area), filename_bar, FALSE, FALSE, 0);
      gtk_box_reorder_child (GTK_BOX (content_area), filename_bar, 0);

      g_signal_connect (dialog, "selection-changed",
                        G_CALLBACK (on_selection_changed), handle);

      /* Hide GTK's built-in filter combo since we have our own */
      if (handle->filter_combo)
        gtk_container_foreach (GTK_CONTAINER (content_area),
                               hide_builtin_filter_cb, NULL);
    }

  gtk_widget_show (dialog);
  gtk_window_present (GTK_WINDOW (dialog));

  if (external_parent)
    external_window_set_parent_of (external_parent,
                                   gtk_widget_get_window (dialog));


  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  return TRUE;
}

gboolean
file_chooser_init (GDBusConnection *bus,
                   GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_file_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-open-file", G_CALLBACK (handle_open), NULL);
  g_signal_connect (helper, "handle-save-file", G_CALLBACK (handle_open), NULL);
  g_signal_connect (helper, "handle-save-files", G_CALLBACK (handle_open), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
