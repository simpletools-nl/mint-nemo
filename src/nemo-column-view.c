/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

#include <config.h>
#include "nemo-column-view.h"

#include "nemo-application.h"
#include "nemo-view-dnd.h"
#include "nemo-view-factory.h"
#include "nemo-window.h"
#include "nemo-window-slot.h"
#include "nemo-window-pane.h"
#include "nemo-actions.h"

#include <string.h>
#include <eel/eel-vfs-extensions.h>
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <libnemo-private/nemo-clipboard-monitor.h>
#include <libnemo-private/nemo-dnd.h>
#include <libnemo-private/nemo-file.h>
#include <libnemo-private/nemo-file-dnd.h>
#include <libnemo-private/nemo-file-utilities.h>
#include <libnemo-private/nemo-global-preferences.h>
#include <libnemo-private/nemo-icon-info.h>
#include <libnemo-private/nemo-icon-names.h>
#include <libnemo-private/nemo-metadata.h>
#include <libnemo-private/nemo-thumbnails.h>
#include <libnemo-private/nemo-ui-utilities.h>

#define DEFAULT_COLUMN_WIDTH 250

enum {
	COLUMN_ICON = 0,
	COLUMN_NAME,
	COLUMN_FILE,
	COLUMN_IS_DIRECTORY,
	COLUMN_NUM_COLUMNS
};

typedef struct {
	NemoColumnView *view;

	GtkWidget *column_widget;
	GtkWidget *scrolled_window;
	GtkWidget *tree_view;
	GtkListStore *list_store;

	GtkCellRendererPixbuf *icon_renderer;
	GtkCellRendererText *text_renderer;

	NemoDirectory *directory;
	NemoFile *directory_file;
	GFile *location;

	gulong files_added_id;
	gulong files_changed_id;
	gulong done_loading_id;

	gpointer monitor_client;

	gboolean loading;
} NemoColumnViewColumn;

struct _NemoColumnViewPriv {
	GtkWidget *columns_container;
	GList *columns;

	NemoZoomLevel zoom_level;

	GList *current_selection;
	gint current_selection_count;

	GtkActionGroup *column_action_group;
	guint column_merge_id;

	gboolean show_hidden_files;

	NemoFileSortType sort_type;
	gboolean sort_reversed;
};

G_DEFINE_TYPE (NemoColumnView, nemo_column_view, NEMO_TYPE_VIEW);

#define parent_class nemo_column_view_parent_class

static void column_view_column_load_directory (NemoColumnViewColumn *col, GFile *location);
static void column_view_column_free (NemoColumnViewColumn *col);
static void column_view_column_clear (NemoColumnViewColumn *col);
static void column_view_rebuild_after_column (NemoColumnView *view, gint column_index);
static void column_view_update_selection (NemoColumnView *view);
static void column_view_clear (NemoView *nemo_view);
static void column_view_column_files_added_cb (NemoDirectory *directory, GList *files, gpointer user_data);
static void column_view_resort_all (NemoColumnView *view);
static void column_view_update_sort_actions (NemoColumnView *view);
static void column_view_sort_radio_callback (GtkAction *action, GtkRadioAction *current, gpointer user_data);
static void column_view_reversed_order_callback (GtkToggleAction *action, gpointer user_data);
static void column_view_sort_order_changed_cb (GSettings *settings, gchar *key, gpointer user_data);
static void column_view_sort_reverse_changed_cb (GSettings *settings, gchar *key, gpointer user_data);

typedef struct {
	const char *nick;
	NemoFileSortType type;
} ColumnViewSortMap;

static const ColumnViewSortMap column_view_sort_map[] = {
	{ "name", NEMO_FILE_SORT_BY_DISPLAY_NAME },
	{ "size", NEMO_FILE_SORT_BY_SIZE },
	{ "type", NEMO_FILE_SORT_BY_TYPE },
	{ "detailed_type", NEMO_FILE_SORT_BY_DETAILED_TYPE },
	{ "mtime", NEMO_FILE_SORT_BY_MTIME },
	{ "atime", NEMO_FILE_SORT_BY_ATIME },
	{ "trash-time", NEMO_FILE_SORT_BY_TRASHED_TIME },
};

static NemoFileSortType
column_view_sort_type_from_nick (const char *nick)
{
	guint i;

	if (nick == NULL) {
		return NEMO_FILE_SORT_BY_DISPLAY_NAME;
	}

	for (i = 0; i < G_N_ELEMENTS (column_view_sort_map); i++) {
		if (strcmp (nick, column_view_sort_map[i].nick) == 0) {
			return column_view_sort_map[i].type;
		}
	}

	return NEMO_FILE_SORT_BY_DISPLAY_NAME;
}

static const char *
column_view_sort_type_to_nick (NemoFileSortType type)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (column_view_sort_map); i++) {
		if (type == column_view_sort_map[i].type) {
			return column_view_sort_map[i].nick;
		}
	}

	return "name";
}

static void
column_view_on_row_activated (GtkTreeView *tree_view,
			      GtkTreePath *path,
			      GtkTreeViewColumn *column,
			      gpointer user_data);
static void
column_view_on_cursor_changed (GtkTreeView *tree_view, gpointer user_data);
static void
column_view_on_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean
column_view_on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data);

static gint
column_view_row_compare_files (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);
	NemoFile *file_a = NULL;
	NemoFile *file_b = NULL;
	gint result;

	gtk_tree_model_get (model, a, COLUMN_FILE, &file_a, -1);
	gtk_tree_model_get (model, b, COLUMN_FILE, &file_b, -1);

	if (file_a == NULL) {
		return (file_b == NULL) ? 0 : 1;
	}
	if (file_b == NULL) {
		return -1;
	}

	result = nemo_file_compare_for_sort (file_a, file_b, view->priv->sort_type,
					     nemo_view_should_sort_directories_first (NEMO_VIEW (view)),
					     nemo_view_should_sort_favorites_first (NEMO_VIEW (view)),
					     view->priv->sort_reversed,
					     NULL);

	return result;
}

static NemoColumnViewColumn *
column_view_column_new (NemoColumnView *view)
{
	NemoColumnViewColumn *col;
	GtkTreeSelection *selection;

	col = g_new0 (NemoColumnViewColumn, 1);
	col->view = view;

	col->column_widget = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

	col->scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (col->scrolled_window),
					GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (col->scrolled_window),
					     GTK_SHADOW_IN);
	gtk_box_pack_start (GTK_BOX (col->column_widget), col->scrolled_window, TRUE, TRUE, 0);

	col->list_store = gtk_list_store_new (COLUMN_NUM_COLUMNS,
					      GDK_TYPE_PIXBUF,
					      G_TYPE_STRING,
					      G_TYPE_POINTER,
					      G_TYPE_BOOLEAN);

	gtk_tree_sortable_set_sort_func (GTK_TREE_SORTABLE (col->list_store), COLUMN_NAME,
					 column_view_row_compare_files, view, NULL);
	gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (col->list_store), COLUMN_NAME,
					      GTK_SORT_ASCENDING);

	col->tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (col->list_store));
	gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (col->tree_view), FALSE);
	gtk_tree_view_set_enable_search (GTK_TREE_VIEW (col->tree_view), TRUE);
	gtk_tree_view_set_search_column (GTK_TREE_VIEW (col->tree_view), COLUMN_NAME);

	gtk_container_add (GTK_CONTAINER (col->scrolled_window), col->tree_view);

	{
		GtkTreeViewColumn *tv_column;

		tv_column = gtk_tree_view_column_new ();

		col->icon_renderer = (GtkCellRendererPixbuf *) gtk_cell_renderer_pixbuf_new ();
		gtk_tree_view_column_pack_start (tv_column,
						 GTK_CELL_RENDERER (col->icon_renderer), FALSE);
		gtk_tree_view_column_set_attributes (tv_column,
						     GTK_CELL_RENDERER (col->icon_renderer),
						     "pixbuf", COLUMN_ICON,
						     NULL);

		col->text_renderer = (GtkCellRendererText *) gtk_cell_renderer_text_new ();
		g_object_set (col->text_renderer,
			      "ellipsize", PANGO_ELLIPSIZE_END,
			      NULL);
		gtk_tree_view_column_pack_start (tv_column,
						 GTK_CELL_RENDERER (col->text_renderer), TRUE);
		gtk_tree_view_column_set_attributes (tv_column,
						     GTK_CELL_RENDERER (col->text_renderer),
						     "text", COLUMN_NAME,
						     NULL);
		gtk_tree_view_column_set_min_width (tv_column, 100);
		gtk_tree_view_column_set_resizable (tv_column, TRUE);
		gtk_tree_view_append_column (GTK_TREE_VIEW (col->tree_view), tv_column);
	}

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);

	g_signal_connect (col->tree_view, "row-activated",
			  G_CALLBACK (column_view_on_row_activated), view);
	g_signal_connect (col->tree_view, "cursor-changed",
			  G_CALLBACK (column_view_on_cursor_changed), view);
	g_signal_connect (col->tree_view, "button-press-event",
			  G_CALLBACK (column_view_on_button_press), view);
	g_signal_connect (col->tree_view, "key-press-event",
			  G_CALLBACK (column_view_on_key_press), view);

	g_object_unref (col->list_store);

	return col;
}

static void
column_view_column_free (NemoColumnViewColumn *col)
{
	if (col == NULL) return;

	column_view_column_clear (col);

	if (col->column_widget != NULL) {
		gtk_widget_destroy (col->column_widget);
		col->column_widget = NULL;
	}

	if (col->directory != NULL) {
		nemo_directory_cancel_callback (col->directory,
						(NemoDirectoryCallback) column_view_column_files_added_cb,
						col);
		if (col->files_added_id > 0) {
			g_signal_handler_disconnect (col->directory, col->files_added_id);
			col->files_added_id = 0;
		}
		if (col->files_changed_id > 0) {
			g_signal_handler_disconnect (col->directory, col->files_changed_id);
			col->files_changed_id = 0;
		}
		if (col->done_loading_id > 0) {
			g_signal_handler_disconnect (col->directory, col->done_loading_id);
			col->done_loading_id = 0;
		}
		if (col->monitor_client != NULL) {
			nemo_directory_file_monitor_remove (col->directory, col->monitor_client);
			col->monitor_client = NULL;
		}
		nemo_directory_unref (col->directory);
		col->directory = NULL;
	}

	if (col->directory_file != NULL) {
		nemo_file_unref (col->directory_file);
		col->directory_file = NULL;
	}

	if (col->location != NULL) {
		g_object_unref (col->location);
		col->location = NULL;
	}

	g_free (col);
}

static void
column_view_column_clear (NemoColumnViewColumn *col)
{
	GtkTreeIter iter;
	NemoFile *file;
	gboolean valid;

	if (col == NULL || col->list_store == NULL) return;

	valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter);
	while (valid) {
		gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
				    COLUMN_FILE, &file, -1);
		if (file != NULL) {
			nemo_file_unref (file);
		}
		valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
	}

	gtk_list_store_clear (col->list_store);
}

static gboolean
column_view_column_add_file (NemoColumnViewColumn *col, NemoFile *file)
{
	GtkTreeIter iter;
	gchar *name;
	gboolean is_dir;
	GdkPixbuf *icon;
	GtkTreeIter existing;
	NemoFile *existing_file;
	gboolean valid;

	if (col == NULL || file == NULL) return FALSE;

	if (!col->view->priv->show_hidden_files && nemo_file_is_hidden_file (file)) {
		return FALSE;
	}

	valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &existing);
	while (valid) {
		gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &existing,
				    COLUMN_FILE, &existing_file, -1);
		if (existing_file == file) {
			return FALSE;
		}
		valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &existing);
	}

	name = nemo_file_get_display_name (file);
	is_dir = (nemo_file_get_file_type (file) == G_FILE_TYPE_DIRECTORY);

	icon = nemo_file_get_icon_pixbuf (file,
					  nemo_get_icon_size_for_zoom_level (col->view->priv->zoom_level),
					  TRUE, 1,
					  NEMO_FILE_ICON_FLAGS_NONE);

	nemo_file_ref (file);

	gtk_list_store_append (col->list_store, &iter);
	gtk_list_store_set (col->list_store, &iter,
			    COLUMN_ICON, icon,
			    COLUMN_NAME, name,
			    COLUMN_FILE, file,
			    COLUMN_IS_DIRECTORY, is_dir,
			    -1);

		if (icon != NULL) {
			g_object_unref (icon);
		}

	return TRUE;
}

static void
column_view_column_files_added_cb (NemoDirectory *directory,
				   GList *files,
				   gpointer user_data)
{
	NemoColumnViewColumn *col = user_data;
	GList *l;

	for (l = files; l != NULL; l = l->next) {
		column_view_column_add_file (col, NEMO_FILE (l->data));
	}
}

static void
column_view_column_files_changed_cb (NemoDirectory *directory,
				     GList *files,
				     gpointer user_data)
{
	NemoColumnViewColumn *col = user_data;
	GList *l;
	GtkTreeIter iter;
	gboolean valid;
	NemoFile *existing_file;

	for (l = files; l != NULL; l = l->next) {
		NemoFile *changed_file = NEMO_FILE (l->data);

		valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter);
		while (valid) {
			gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
					    COLUMN_FILE, &existing_file, -1);
			if (existing_file == changed_file) {
				GdkPixbuf *icon;
				gchar *name;
				gboolean is_dir;

				name = nemo_file_get_display_name (changed_file);
				is_dir = (nemo_file_get_file_type (changed_file) == G_FILE_TYPE_DIRECTORY);
				icon = nemo_file_get_icon_pixbuf (changed_file,
								  nemo_get_icon_size_for_zoom_level (col->view->priv->zoom_level),
								  TRUE, 1,
								  NEMO_FILE_ICON_FLAGS_NONE);

				gtk_list_store_set (col->list_store, &iter,
						    COLUMN_ICON, icon,
						    COLUMN_NAME, name,
						    COLUMN_IS_DIRECTORY, is_dir,
						    -1);

				if (icon != NULL)
					g_object_unref (icon);
				g_free (name);
				break;
			}
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
		}
	}
}

static void
column_view_column_done_loading_cb (NemoDirectory *directory, gpointer user_data)
{
	NemoColumnViewColumn *col = user_data;
	col->loading = FALSE;
}

static void
column_view_column_load_directory (NemoColumnViewColumn *col, GFile *location)
{
	gboolean same_location;

	if (col == NULL || location == NULL) return;

	same_location = (col->location == location);

	if (!same_location) {
		if (col->directory_file != NULL) {
			nemo_file_unref (col->directory_file);
			col->directory_file = NULL;
		}

		if (col->location != NULL) {
			g_object_unref (col->location);
			col->location = NULL;
		}
	}

	if (col->directory != NULL) {
		nemo_directory_cancel_callback (col->directory,
						(NemoDirectoryCallback) column_view_column_files_added_cb,
						col);
		if (col->files_added_id > 0) {
			g_signal_handler_disconnect (col->directory, col->files_added_id);
			col->files_added_id = 0;
		}
		if (col->files_changed_id > 0) {
			g_signal_handler_disconnect (col->directory, col->files_changed_id);
			col->files_changed_id = 0;
		}
		if (col->done_loading_id > 0) {
			g_signal_handler_disconnect (col->directory, col->done_loading_id);
			col->done_loading_id = 0;
		}
		if (col->monitor_client != NULL) {
			nemo_directory_file_monitor_remove (col->directory, col->monitor_client);
			col->monitor_client = NULL;
		}
		nemo_directory_unref (col->directory);
		col->directory = NULL;
	}

	col->location = g_object_ref (location);
	column_view_column_clear (col);
	col->loading = TRUE;

	col->directory = nemo_directory_get (location);

	col->directory_file = nemo_file_get (location);

	col->done_loading_id = g_signal_connect (col->directory, "done_loading",
						 G_CALLBACK (column_view_column_done_loading_cb), col);

	nemo_directory_call_when_ready (col->directory,
					NEMO_FILE_ATTRIBUTES_FOR_ICON |
					NEMO_FILE_ATTRIBUTE_INFO |
					NEMO_FILE_ATTRIBUTE_LINK_INFO,
					FALSE,
					column_view_column_files_added_cb, col);

	{
		GList *fl;
		fl = nemo_directory_get_file_list (col->directory);
		if (fl != NULL) {
			column_view_column_files_added_cb (col->directory, fl, col);
			nemo_file_list_free (fl);
		}
	}

	{
		NemoFileAttributes attrs;
		attrs = NEMO_FILE_ATTRIBUTES_FOR_ICON |
		        NEMO_FILE_ATTRIBUTE_INFO |
		        NEMO_FILE_ATTRIBUTE_LINK_INFO |
		        NEMO_FILE_ATTRIBUTE_DIRECTORY_ITEM_COUNT;

		nemo_directory_file_monitor_add (col->directory, &col->directory,
		                                 col->view->priv->show_hidden_files,
		                                 attrs,
		                                 column_view_column_files_added_cb, col);
		col->monitor_client = &col->directory;

		col->files_added_id = g_signal_connect (col->directory, "files_added",
							G_CALLBACK (column_view_column_files_added_cb), col);
		col->files_changed_id = g_signal_connect (col->directory, "files_changed",
							  G_CALLBACK (column_view_column_files_changed_cb), col);
	}

}

static void
column_view_update_address_bar (NemoColumnView *view, GFile *location)
{
	NemoWindowSlot *slot;
	GFile *old_location;
	gchar *from_uri = NULL;
	gchar *to_uri;

	slot = nemo_view_get_nemo_window_slot (NEMO_VIEW (view));
	if (slot == NULL || location == NULL) return;

	old_location = slot->location;
	slot->location = g_object_ref (location);

	if (old_location != NULL) {
		from_uri = g_file_get_uri (old_location);
		g_object_unref (old_location);
	}
	to_uri = g_file_get_uri (location);

	g_signal_emit_by_name (slot, "location-changed", from_uri, to_uri);

	g_free (from_uri);
	g_free (to_uri);

	nemo_window_pane_sync_location_widgets (slot->pane);
}

static void
column_view_do_popup_menu (NemoColumnView *view, GtkTreeView *tree_view, GdkEventButton *event)
{
	if (tree_view != NULL) {
		GtkTreeSelection *sel = gtk_tree_view_get_selection (tree_view);
		if (gtk_tree_selection_count_selected_rows (sel) > 0) {
			nemo_view_pop_up_selection_context_menu (NEMO_VIEW (view), event);
			return;
		}
	}
	nemo_view_pop_up_background_context_menu (NEMO_VIEW (view), event);
}

static void
column_view_clear_left_columns_selection (NemoColumnView *view)
{
	GList *l;
	gboolean is_last = TRUE;

	for (l = g_list_last (view->priv->columns); l != NULL; l = l->prev) {
		NemoColumnViewColumn *col = l->data;
		GtkTreeSelection *sel;

		sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
		if (is_last) {
			is_last = FALSE;
		} else {
			gtk_tree_selection_unselect_all (sel);
		}
	}
}

static void
column_view_rebuild_after_column (NemoColumnView *view, gint column_index)
{
	while (g_list_length (view->priv->columns) > column_index + 1) {
		NemoColumnViewColumn *col = g_list_last (view->priv->columns)->data;
		view->priv->columns = g_list_remove (view->priv->columns, col);
		column_view_column_free (col);
	}
}

static NemoColumnViewColumn *
column_view_append_column (NemoColumnView *view)
{
	NemoColumnViewColumn *col;

	col = column_view_column_new (view);
	gtk_box_pack_start (GTK_BOX (view->priv->columns_container),
			    col->column_widget, FALSE, TRUE, 0);
	gtk_widget_set_size_request (col->column_widget, DEFAULT_COLUMN_WIDTH, -1);

	view->priv->columns = g_list_append (view->priv->columns, col);
	gtk_widget_show_all (col->column_widget);

	return col;
}

static NemoColumnViewColumn *
column_view_get_column_at_index (NemoColumnView *view, gint index)
{
	return g_list_nth_data (view->priv->columns, index);
}

static void
column_view_on_row_activated (GtkTreeView *tree_view,
			      GtkTreePath *path,
			      GtkTreeViewColumn *column,
			      gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);
	GtkTreeModel *model;
	GtkTreeIter iter;
	NemoFile *file;

	model = gtk_tree_view_get_model (tree_view);
	if (!gtk_tree_model_get_iter (model, &iter, path)) return;

	gtk_tree_model_get (model, &iter, COLUMN_FILE, &file, -1);

	if (file != NULL) {
		if (nemo_file_is_directory (file)) {
			gint column_index = -1;
			GList *l;
			gint i;

			for (i = 0, l = view->priv->columns; l != NULL; l = l->next, i++) {
				NemoColumnViewColumn *col = l->data;
				if (col->tree_view == GTK_WIDGET (tree_view)) {
					column_index = i;
					break;
				}
			}

			if (column_index >= 0) {
				GFile *dir_location = nemo_file_get_location (file);
				column_view_rebuild_after_column (view, column_index);
				NemoColumnViewColumn *new_col = column_view_append_column (view);
				column_view_column_load_directory (new_col, dir_location);
				gtk_widget_show_all (new_col->column_widget);
				gtk_widget_queue_resize (view->priv->columns_container);
				column_view_update_address_bar (view, dir_location);
				column_view_clear_left_columns_selection (view);
				g_object_unref (dir_location);
			}
		} else {
			nemo_file_ref (file);
			nemo_view_activate_file (NEMO_VIEW (view), file, 0);
			nemo_file_unref (file);
		}
	}
}

static void
column_view_on_cursor_changed (GtkTreeView *tree_view, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);
	column_view_update_selection (view);
	g_signal_emit_by_name (view, "selection-changed", 0);
}

static void
column_view_on_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);
	GtkTreeView *tree_view = GTK_TREE_VIEW (widget);

	if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
		GtkTreePath *path;

		if (gtk_tree_view_get_path_at_pos (tree_view,
						   (gint) event->x,
						   (gint) event->y,
						   &path, NULL, NULL, NULL)) {
			GtkTreeModel *model;
			GtkTreeIter iter;
			NemoFile *file;
			gboolean is_dir;

			model = gtk_tree_view_get_model (tree_view);

			if (gtk_tree_model_get_iter (model, &iter, path)) {
				gtk_tree_model_get (model, &iter,
						    COLUMN_FILE, &file,
						    COLUMN_IS_DIRECTORY, &is_dir,
						    -1);

				if (is_dir && file != NULL) {
					gint column_index = -1;
					GList *l;
					gint i;

					for (i = 0, l = view->priv->columns; l != NULL; l = l->next, i++) {
						NemoColumnViewColumn *col = l->data;
						if (col->tree_view == GTK_WIDGET (tree_view)) {
							column_index = i;
							break;
						}
					}

					if (column_index >= 0) {
						GFile *dir_location = nemo_file_get_location (file);

						column_view_rebuild_after_column (view, column_index);

						NemoColumnViewColumn *new_col = column_view_append_column (view);
						column_view_column_load_directory (new_col, dir_location);

						gtk_widget_show_all (new_col->column_widget);

						column_view_update_address_bar (view, dir_location);

						g_object_unref (dir_location);

						column_view_clear_left_columns_selection (view);
					}

					column_view_update_selection (view);
					g_signal_emit_by_name (view, "selection-changed", 0);
				}
			}

			gtk_tree_path_free (path);
		}
	}

	if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
		GtkTreePath *right_path;

		if (gtk_tree_view_get_path_at_pos (tree_view,
						   (gint) event->x,
						   (gint) event->y,
						   &right_path, NULL, NULL, NULL)) {
			GtkTreeSelection *sel = gtk_tree_view_get_selection (tree_view);
			if (!gtk_tree_selection_path_is_selected (sel, right_path)) {
				gtk_tree_selection_unselect_all (sel);
				gtk_tree_selection_select_path (sel, right_path);
			}
			gtk_tree_path_free (right_path);
		} else {
			GtkTreeSelection *sel = gtk_tree_view_get_selection (tree_view);
			gtk_tree_selection_unselect_all (sel);
		}

		column_view_update_selection (view);
		column_view_do_popup_menu (view, tree_view, event);
	}
}

static gboolean
column_view_on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);

	if (event->keyval == GDK_KEY_Right) {
		GList *l;
		for (l = view->priv->columns; l != NULL; l = l->next) {
			NemoColumnViewColumn *col = l->data;
			GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
			if (gtk_tree_selection_count_selected_rows (sel) > 0) {
				if (l->next != NULL) {
					NemoColumnViewColumn *next_col = l->next->data;
					gtk_widget_grab_focus (next_col->tree_view);
				}
				return TRUE;
			}
		}
	} else if (event->keyval == GDK_KEY_Left) {
		GList *l;
		for (l = view->priv->columns; l != NULL; l = l->next) {
			NemoColumnViewColumn *col = l->data;
			GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
			if (gtk_tree_selection_count_selected_rows (sel) > 0) {
				if (l->prev != NULL) {
					NemoColumnViewColumn *prev_col = l->prev->data;
					gtk_widget_grab_focus (prev_col->tree_view);
				}
				return TRUE;
			}
		}
	}

	return FALSE;
}

static void
column_view_add_file (NemoView *nemo_view, NemoFile *file, NemoDirectory *directory)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	NemoColumnViewColumn *col = column_view_get_column_at_index (view, 0);

	if (col == NULL) {
		col = column_view_append_column (view);
	}

	column_view_column_add_file (col, file);
}

static void
column_view_remove_file (NemoView *nemo_view, NemoFile *file, NemoDirectory *directory)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *l;

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		GtkTreeIter iter;
		gboolean valid;
		NemoFile *existing_file;

		valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter);
		while (valid) {
			gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
					    COLUMN_FILE, &existing_file, -1);
			if (existing_file == file) {
				nemo_file_unref (file);
				gtk_list_store_remove (col->list_store, &iter);
				return;
			}
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
		}
	}
}

static void
column_view_file_changed (NemoView *nemo_view, NemoFile *file, NemoDirectory *directory)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *l;

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		GtkTreeIter iter;
		gboolean valid;
		NemoFile *existing_file;

		valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter);
		while (valid) {
			gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
					    COLUMN_FILE, &existing_file, -1);
			if (existing_file == file) {
				column_view_column_files_changed_cb (NULL,
								     g_list_prepend (NULL, file),
								     col);
				return;
			}
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
		}
	}
}

static void
column_view_begin_loading (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	NemoColumnViewColumn *col;

	if (view->priv->columns != NULL) {
		column_view_clear (nemo_view);
	}

	col = column_view_append_column (view);

	if (col != NULL && col->directory == NULL) {
		NemoFile *dir_file = nemo_view_get_directory_as_file (nemo_view);
		if (dir_file != NULL) {
			GFile *location = nemo_file_get_location (dir_file);
			column_view_column_load_directory (col, location);
			g_object_unref (location);
		}
	}
}

static void
column_view_end_loading (NemoView *nemo_view, gboolean all_files_seen)
{
}

static void
column_view_clear (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);

	while (view->priv->columns != NULL) {
		NemoColumnViewColumn *col = view->priv->columns->data;
		view->priv->columns = g_list_delete_link (view->priv->columns, view->priv->columns);
		column_view_column_free (col);
	}
}

static GList *
column_view_get_selection (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *selection = NULL;
	NemoColumnViewColumn *col;
	GtkTreeSelection *tree_sel;
	GList *selected_rows;
	GList *r;
	GList *last_node;

	last_node = g_list_last (view->priv->columns);
	if (last_node == NULL) return NULL;

	col = last_node->data;
	tree_sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
	selected_rows = gtk_tree_selection_get_selected_rows (tree_sel, NULL);

	for (r = selected_rows; r != NULL; r = r->next) {
		GtkTreeIter iter;
		NemoFile *file;

		if (gtk_tree_model_get_iter (GTK_TREE_MODEL (col->list_store),
					     &iter, (GtkTreePath *) r->data)) {
			gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
					    COLUMN_FILE, &file, -1);
			if (file != NULL) {
				selection = g_list_prepend (selection, file);
			}
		}
	}

	g_list_free_full (selected_rows, (GDestroyNotify) gtk_tree_path_free);

	selection = g_list_reverse (selection);
	nemo_file_list_ref (selection);
	return selection;
}

static GList *
column_view_peek_selection (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	if (view->priv->current_selection_count == -1)
		column_view_update_selection (view);
	return view->priv->current_selection;
}

static gint
column_view_get_selection_count (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	if (view->priv->current_selection_count == -1)
		column_view_update_selection (view);
	return view->priv->current_selection_count;
}

static void
column_view_update_selection (NemoColumnView *view)
{
	if (view->priv->current_selection != NULL) {
		nemo_file_list_free (view->priv->current_selection);
		view->priv->current_selection = NULL;
		view->priv->current_selection_count = 0;
	}
	view->priv->current_selection = column_view_get_selection (NEMO_VIEW (view));
	view->priv->current_selection_count = g_list_length (view->priv->current_selection);
}

static GList *
column_view_get_selection_for_file_transfer (NemoView *nemo_view)
{
	return column_view_get_selection (nemo_view);
}

static guint
column_view_get_item_count (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	NemoColumnViewColumn *col = column_view_get_column_at_index (view, 0);
	if (col != NULL)
		return gtk_tree_model_iter_n_children (GTK_TREE_MODEL (col->list_store), NULL);
	return 0;
}

static gboolean
column_view_is_empty (NemoView *nemo_view)
{
	return (column_view_get_item_count (nemo_view) == 0);
}

static void
column_view_end_file_changes (NemoView *nemo_view)
{
}

static void
column_view_select_all (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	NemoColumnViewColumn *col = column_view_get_column_at_index (view, 0);
	if (col != NULL) {
		GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
		gtk_tree_selection_select_all (sel);
	}
	column_view_update_selection (view);
	g_signal_emit_by_name (view, "selection-changed", 0);
}

static void
column_view_set_selection (NemoView *nemo_view, GList *selection)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *l;
	NemoColumnViewColumn *col;

	col = column_view_get_column_at_index (view, 0);
	if (col == NULL) return;

	{
		GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
		gtk_tree_selection_unselect_all (sel);
	}

	for (l = selection; l != NULL; l = l->next) {
		NemoFile *file = NEMO_FILE (l->data);
		GtkTreeIter iter;
		gboolean valid;
		NemoFile *existing;

		valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter);
		while (valid) {
			gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
					    COLUMN_FILE, &existing, -1);
			if (existing == file) {
				GtkTreeSelection *sel = gtk_tree_view_get_selection (
					GTK_TREE_VIEW (col->tree_view));
				gtk_tree_selection_select_iter (sel, &iter);
				break;
			}
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
		}
	}

	column_view_update_selection (view);
}

static void
column_view_invert_selection (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *l;

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
		GtkTreeIter iter;
		gboolean valid;

		valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter);
		while (valid) {
			if (gtk_tree_selection_iter_is_selected (sel, &iter))
				gtk_tree_selection_unselect_iter (sel, &iter);
			else
				gtk_tree_selection_select_iter (sel, &iter);
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
		}
	}

	column_view_update_selection (view);
	g_signal_emit_by_name (view, "selection-changed", 0);
}

static void
column_view_refresh_icons (NemoColumnView *view)
{
	GList *l;
	gint size;

	size = nemo_get_icon_size_for_zoom_level (view->priv->zoom_level);

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		GtkTreeIter iter;
		gboolean valid;

		if (col->list_store == NULL) {
			continue;
		}

		valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter);
		while (valid) {
			NemoFile *file;
			GdkPixbuf *icon;

			gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
					    COLUMN_FILE, &file, -1);

			if (file != NULL) {
				icon = nemo_file_get_icon_pixbuf (file, size, TRUE, 1,
								  NEMO_FILE_ICON_FLAGS_NONE);
				gtk_list_store_set (col->list_store, &iter,
						    COLUMN_ICON, icon, -1);
				if (icon != NULL) {
					g_object_unref (icon);
				}
			}

			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
		}
	}
}

static void
column_view_bump_zoom_level (NemoView *nemo_view, int zoom_increment)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);

	view->priv->zoom_level += zoom_increment;
	if (view->priv->zoom_level < NEMO_ZOOM_LEVEL_SMALLEST)
		view->priv->zoom_level = NEMO_ZOOM_LEVEL_SMALLEST;
	if (view->priv->zoom_level > NEMO_ZOOM_LEVEL_LARGEST)
		view->priv->zoom_level = NEMO_ZOOM_LEVEL_LARGEST;

	column_view_refresh_icons (view);
}

static void
column_view_zoom_to_level (NemoView *nemo_view, NemoZoomLevel zoom_level)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	view->priv->zoom_level = zoom_level;

	column_view_refresh_icons (view);
}

static NemoZoomLevel
column_view_get_zoom_level (NemoView *nemo_view)
{
	return NEMO_COLUMN_VIEW (nemo_view)->priv->zoom_level;
}

static void
column_view_restore_default_zoom_level (NemoView *nemo_view)
{
	column_view_zoom_to_level (nemo_view, NEMO_ZOOM_LEVEL_STANDARD);
}

static NemoZoomLevel
column_view_get_default_zoom_level (NemoView *nemo_view)
{
	return NEMO_ZOOM_LEVEL_STANDARD;
}

static gboolean
column_view_can_zoom_in (NemoView *nemo_view)
{
	return NEMO_COLUMN_VIEW (nemo_view)->priv->zoom_level < NEMO_ZOOM_LEVEL_LARGEST;
}

static gboolean
column_view_can_zoom_out (NemoView *nemo_view)
{
	return NEMO_COLUMN_VIEW (nemo_view)->priv->zoom_level > NEMO_ZOOM_LEVEL_SMALLEST;
}

static void
column_view_reveal_selection (NemoView *nemo_view)
{
}

static void
column_view_reset_to_defaults (NemoView *nemo_view)
{
	column_view_restore_default_zoom_level (nemo_view);
}

static gboolean
column_view_using_manual_layout (NemoView *nemo_view)
{
	return FALSE;
}

static const char *
column_view_get_view_id (NemoView *nemo_view)
{
	return NEMO_COLUMN_VIEW_ID;
}

static char *
column_view_get_first_visible_file (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	NemoColumnViewColumn *col = column_view_get_column_at_index (view, 0);

	if (col != NULL) {
		GtkTreeIter iter;
		if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter)) {
			NemoFile *file;
			gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
					    COLUMN_FILE, &file, -1);
			if (file != NULL) {
				return nemo_file_get_uri (file);
			}
		}
	}
	return NULL;
}

static void
column_view_scroll_to_file (NemoView *nemo_view, const char *uri)
{
}

static void
column_view_start_renaming_file (NemoView *nemo_view, NemoFile *file, gboolean select_all)
{
}

static int
column_view_compare_files (NemoView *nemo_view, NemoFile *a, NemoFile *b)
{
	char *name_a, *name_b;
	int result;

	name_a = nemo_file_get_display_name (a);
	name_b = nemo_file_get_display_name (b);
	result = g_utf8_collate (name_a, name_b);
	g_free (name_a);
	g_free (name_b);
	return result;
}

static void
column_view_click_policy_changed (NemoView *nemo_view)
{
}

static void
column_view_sort_directories_first_changed (NemoView *nemo_view)
{
}

static void
column_view_sort_favorites_first_changed (NemoView *nemo_view)
{
}

static const GtkActionEntry column_view_action_entries[] = {
	/* name, stock id, label */  { "Arrange Items", NULL, N_("Arran_ge Items") },
};

static const GtkToggleActionEntry column_view_toggle_entries[] = {
	/* name, stock id */      { "Reversed Order", NULL,
	/* label, accelerator */    N_("Re_versed Order"), NULL,
	/* tooltip */               N_("Display files in the opposite order"),
	                            G_CALLBACK (column_view_reversed_order_callback),
	                            0 },
};

static const GtkRadioActionEntry column_view_sort_entries[] = {
	{ "Sort by Name", NULL,
	  N_("By _Name"), NULL,
	  N_("Sort files by name"),
	  NEMO_FILE_SORT_BY_DISPLAY_NAME },
	{ "Sort by Size", NULL,
	  N_("By _Size"), NULL,
	  N_("Sort files by size"),
	  NEMO_FILE_SORT_BY_SIZE },
	{ "Sort by Type", NULL,
	  N_("By _Type"), NULL,
	  N_("Sort files by type"),
	  NEMO_FILE_SORT_BY_TYPE },
	{ "Sort by Detailed Type", NULL,
	  N_("By _Detailed Type"), NULL,
	  N_("Sort files by detailed type"),
	  NEMO_FILE_SORT_BY_DETAILED_TYPE },
	{ "Sort by Modification Date", NULL,
	  N_("By Modification _Date"), NULL,
	  N_("Sort files by modification date"),
	  NEMO_FILE_SORT_BY_MTIME },
	{ "Sort by Access Date", NULL,
	  N_("By _Access Date"), NULL,
	  N_("Sort files by last access date"),
	  NEMO_FILE_SORT_BY_ATIME },
	{ "Sort by Trash Time", NULL,
	  N_("By T_rash Time"), NULL,
	  N_("Sort files by trash time"),
	  NEMO_FILE_SORT_BY_TRASHED_TIME },
};

static void
column_view_resort_all (NemoColumnView *view)
{
	GList *l;

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		if (col->list_store == NULL) {
			continue;
		}
		gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (col->list_store),
						      GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
						      GTK_SORT_ASCENDING);
		gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (col->list_store),
						      COLUMN_NAME,
						      GTK_SORT_ASCENDING);
	}
}

static void
column_view_update_sort_actions (NemoColumnView *view)
{
	GtkAction *action;
	NemoFile *file;

	if (view->priv->column_action_group == NULL) {
		return;
	}

	action = gtk_action_group_get_action (view->priv->column_action_group, "Sort by Name");
	if (action != NULL) {
		gtk_radio_action_set_current_value (GTK_RADIO_ACTION (action), view->priv->sort_type);
	}

	action = gtk_action_group_get_action (view->priv->column_action_group, "Reversed Order");
	if (action != NULL) {
		gtk_toggle_action_set_active (GTK_TOGGLE_ACTION (action), view->priv->sort_reversed);
	}

	action = gtk_action_group_get_action (view->priv->column_action_group, "Sort by Trash Time");
	if (action != NULL) {
		file = nemo_view_get_directory_as_file (NEMO_VIEW (view));
		if (file != NULL && nemo_file_is_in_trash (file)) {
			gtk_action_set_visible (action, TRUE);
		} else {
			gtk_action_set_visible (action, FALSE);
		}
	}
}

static void
column_view_sort_radio_callback (GtkAction *action, GtkRadioAction *current, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);

	view->priv->sort_type = gtk_radio_action_get_current_value (current);
	g_settings_set_string (nemo_preferences,
			       NEMO_PREFERENCES_DEFAULT_SORT_ORDER,
			       column_view_sort_type_to_nick (view->priv->sort_type));
	column_view_resort_all (view);
}

static void
column_view_reversed_order_callback (GtkToggleAction *action, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);

	view->priv->sort_reversed = gtk_toggle_action_get_active (action);
	g_settings_set_boolean (nemo_preferences,
				NEMO_PREFERENCES_DEFAULT_SORT_IN_REVERSE_ORDER,
				view->priv->sort_reversed);
	column_view_resort_all (view);
}

static void
column_view_sort_order_changed_cb (GSettings *settings, gchar *key, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);
	char *sort_order;

	sort_order = g_settings_get_string (settings, key);
	view->priv->sort_type = column_view_sort_type_from_nick (sort_order);
	g_free (sort_order);

	column_view_resort_all (view);
}

static void
column_view_sort_reverse_changed_cb (GSettings *settings, gchar *key, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);

	view->priv->sort_reversed = g_settings_get_boolean (settings, key);

	column_view_resort_all (view);
}

static void
column_view_merge_menus (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GtkUIManager *ui_manager;
	GtkActionGroup *action_group;

	NEMO_VIEW_CLASS (parent_class)->merge_menus (nemo_view);

	ui_manager = nemo_view_get_ui_manager (nemo_view);
	action_group = gtk_action_group_new ("ColumnViewActions");
	gtk_action_group_set_translation_domain (action_group, GETTEXT_PACKAGE);

	gtk_action_group_add_actions (action_group,
				      column_view_action_entries, G_N_ELEMENTS (column_view_action_entries),
				      view);
	gtk_action_group_add_toggle_actions (action_group,
					     column_view_toggle_entries, G_N_ELEMENTS (column_view_toggle_entries),
					     view);
	gtk_action_group_add_radio_actions (action_group,
					    column_view_sort_entries, G_N_ELEMENTS (column_view_sort_entries),
					    -1,
					    G_CALLBACK (column_view_sort_radio_callback),
					    view);

	view->priv->column_action_group = action_group;
	gtk_ui_manager_insert_action_group (ui_manager, action_group, 0);
	g_object_unref (action_group); /* owned by ui manager */

	view->priv->column_merge_id =
		gtk_ui_manager_add_ui_from_resource (ui_manager, "/org/nemo/nemo-column-view-ui.xml", NULL);

	column_view_update_sort_actions (view);
}

static void
column_view_unmerge_menus (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GtkUIManager *ui_manager;

	NEMO_VIEW_CLASS (parent_class)->unmerge_menus (nemo_view);

	ui_manager = nemo_view_get_ui_manager (nemo_view);
	if (ui_manager != NULL) {
		nemo_ui_unmerge_ui (ui_manager,
				    &view->priv->column_merge_id,
				    &view->priv->column_action_group);
	}
}

static void
column_view_update_menus (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);

	NEMO_VIEW_CLASS (parent_class)->update_menus (nemo_view);

	column_view_update_sort_actions (view);
}

static gboolean
column_view_popup_menu (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *l;
	GtkTreeView *focused = NULL;

	column_view_update_selection (view);

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		if (gtk_widget_has_focus (GTK_WIDGET (col->tree_view))) {
			focused = GTK_TREE_VIEW (col->tree_view);
			break;
		}
	}

	column_view_do_popup_menu (view, focused, NULL);
	return TRUE;
}

static gboolean
column_view_button_press_event (GtkWidget *widget, GdkEventButton *event)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (widget);
	if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
		GtkWidget *child = gtk_get_event_widget ((GdkEvent *) event);
		if (child != NULL && child != widget) {
			GList *l;
			for (l = view->priv->columns; l != NULL; l = l->next) {
				NemoColumnViewColumn *col = l->data;
				if (GTK_WIDGET (col->tree_view) == child ||
				    gtk_widget_is_ancestor (child, GTK_WIDGET (col->tree_view))) {
					column_view_update_selection (view);
					column_view_do_popup_menu (view, GTK_TREE_VIEW (col->tree_view), event);
					return TRUE;
				}
			}
		}
		column_view_update_selection (view);
		column_view_do_popup_menu (view, NULL, event);
		return TRUE;
	}
	return GTK_WIDGET_CLASS (parent_class)->button_press_event (widget, event);
}

static void
column_view_hidden_files_changed_cb (GSettings *settings, gchar *key, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);
	GList *l;

	view->priv->show_hidden_files = g_settings_get_boolean (nemo_preferences,
								NEMO_PREFERENCES_SHOW_HIDDEN_FILES);

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		if (col->location != NULL) {
			GFile *loc = g_object_ref (col->location);
			column_view_column_load_directory (col, loc);
			g_object_unref (loc);
		}
	}
}

static void
nemo_column_view_init (NemoColumnView *view)
{
	view->priv = G_TYPE_INSTANCE_GET_PRIVATE (view, NEMO_TYPE_COLUMN_VIEW,
						   NemoColumnViewPriv);

	view->priv->columns_container = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_container_add (GTK_CONTAINER (view), view->priv->columns_container);
	gtk_widget_show (view->priv->columns_container);

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (view),
					GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);

	view->priv->zoom_level = NEMO_ZOOM_LEVEL_STANDARD;
	view->priv->current_selection = NULL;
	view->priv->current_selection_count = -1;
	view->priv->show_hidden_files = g_settings_get_boolean (nemo_preferences,
								NEMO_PREFERENCES_SHOW_HIDDEN_FILES);

	{
		char *sort_order;

		sort_order = g_settings_get_string (nemo_preferences,
						    NEMO_PREFERENCES_DEFAULT_SORT_ORDER);
		view->priv->sort_type = column_view_sort_type_from_nick (sort_order);
		g_free (sort_order);
	}
	view->priv->sort_reversed = g_settings_get_boolean (nemo_preferences,
							    NEMO_PREFERENCES_DEFAULT_SORT_IN_REVERSE_ORDER);

	g_signal_connect (nemo_preferences,
			  "changed::" NEMO_PREFERENCES_SHOW_HIDDEN_FILES,
			  G_CALLBACK (column_view_hidden_files_changed_cb),
			  view);
	g_signal_connect (nemo_preferences,
			  "changed::" NEMO_PREFERENCES_DEFAULT_SORT_ORDER,
			  G_CALLBACK (column_view_sort_order_changed_cb),
			  view);
	g_signal_connect (nemo_preferences,
			  "changed::" NEMO_PREFERENCES_DEFAULT_SORT_IN_REVERSE_ORDER,
			  G_CALLBACK (column_view_sort_reverse_changed_cb),
			  view);

	gtk_widget_add_events (GTK_WIDGET (view), GDK_BUTTON_PRESS_MASK);

	g_signal_connect (view, "popup-menu",
			  G_CALLBACK (column_view_popup_menu), view);
}

static void
column_view_finalize (GObject *object)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (object);
	GList *l;

	g_signal_handlers_disconnect_by_func (nemo_preferences,
					      column_view_hidden_files_changed_cb, view);
	g_signal_handlers_disconnect_by_func (nemo_preferences,
					      column_view_sort_order_changed_cb, view);
	g_signal_handlers_disconnect_by_func (nemo_preferences,
					      column_view_sort_reverse_changed_cb, view);

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		if (col->directory != NULL) {
			nemo_directory_cancel_callback (col->directory,
							(NemoDirectoryCallback) column_view_column_files_added_cb,
							col);
			if (col->files_added_id > 0) {
				g_signal_handler_disconnect (col->directory, col->files_added_id);
				col->files_added_id = 0;
			}
			if (col->files_changed_id > 0) {
				g_signal_handler_disconnect (col->directory, col->files_changed_id);
				col->files_changed_id = 0;
			}
			if (col->done_loading_id > 0) {
				g_signal_handler_disconnect (col->directory, col->done_loading_id);
				col->done_loading_id = 0;
			}
			if (col->monitor_client != NULL) {
				nemo_directory_file_monitor_remove (col->directory, col->monitor_client);
				col->monitor_client = NULL;
			}
			nemo_directory_unref (col->directory);
		}
		if (col->directory_file != NULL) {
			nemo_file_unref (col->directory_file);
		}
		if (col->location != NULL) {
			g_object_unref (col->location);
		}
		g_free (col);
	}
	g_list_free (view->priv->columns);
	view->priv->columns = NULL;

	if (view->priv->current_selection != NULL) {
		g_list_free (view->priv->current_selection);
		view->priv->current_selection = NULL;
	}

	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
nemo_column_view_class_init (NemoColumnViewClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	NemoViewClass *nemo_view_class = NEMO_VIEW_CLASS (class);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

	g_type_class_add_private (class, sizeof (NemoColumnViewPriv));

	object_class->finalize = column_view_finalize;

	widget_class->button_press_event = column_view_button_press_event;

	nemo_view_class->add_file = column_view_add_file;
	nemo_view_class->remove_file = column_view_remove_file;
	nemo_view_class->file_changed = column_view_file_changed;
	nemo_view_class->begin_loading = column_view_begin_loading;
	nemo_view_class->end_loading = column_view_end_loading;
	nemo_view_class->clear = column_view_clear;
	nemo_view_class->get_selection = column_view_get_selection;
	nemo_view_class->peek_selection = column_view_peek_selection;
	nemo_view_class->get_selection_count = column_view_get_selection_count;
	nemo_view_class->get_selection_for_file_transfer = column_view_get_selection_for_file_transfer;
	nemo_view_class->get_item_count = column_view_get_item_count;
	nemo_view_class->is_empty = column_view_is_empty;
	nemo_view_class->end_file_changes = column_view_end_file_changes;
	nemo_view_class->select_all = column_view_select_all;
	nemo_view_class->set_selection = column_view_set_selection;
	nemo_view_class->invert_selection = column_view_invert_selection;
	nemo_view_class->bump_zoom_level = column_view_bump_zoom_level;
	nemo_view_class->zoom_to_level = column_view_zoom_to_level;
	nemo_view_class->get_zoom_level = column_view_get_zoom_level;
	nemo_view_class->restore_default_zoom_level = column_view_restore_default_zoom_level;
	nemo_view_class->get_default_zoom_level = column_view_get_default_zoom_level;
	nemo_view_class->can_zoom_in = column_view_can_zoom_in;
	nemo_view_class->can_zoom_out = column_view_can_zoom_out;
	nemo_view_class->reveal_selection = column_view_reveal_selection;
	nemo_view_class->reset_to_defaults = column_view_reset_to_defaults;
	nemo_view_class->using_manual_layout = column_view_using_manual_layout;
	nemo_view_class->get_view_id = column_view_get_view_id;
	nemo_view_class->get_first_visible_file = column_view_get_first_visible_file;
	nemo_view_class->scroll_to_file = column_view_scroll_to_file;
	nemo_view_class->start_renaming_file = column_view_start_renaming_file;
	nemo_view_class->compare_files = column_view_compare_files;
	nemo_view_class->click_policy_changed = column_view_click_policy_changed;
	nemo_view_class->sort_directories_first_changed = column_view_sort_directories_first_changed;
	nemo_view_class->sort_favorites_first_changed = column_view_sort_favorites_first_changed;
	nemo_view_class->merge_menus = column_view_merge_menus;
	nemo_view_class->unmerge_menus = column_view_unmerge_menus;
	nemo_view_class->update_menus = column_view_update_menus;
}

static NemoView *
nemo_column_view_create (NemoWindowSlot *slot)
{
	NemoColumnView *view;
	view = g_object_new (NEMO_TYPE_COLUMN_VIEW,
			     "window-slot", slot,
			     NULL);
	return NEMO_VIEW (view);
}

static gboolean
nemo_column_view_supports_uri (const char *uri,
			       GFileType file_type,
			       const char *mime_type)
{
	if (file_type == G_FILE_TYPE_DIRECTORY) return TRUE;
	if (g_str_has_prefix (uri, "trash:")) return TRUE;
	if (g_str_has_prefix (uri, EEL_SEARCH_URI)) return TRUE;
	return FALSE;
}

static NemoViewInfo nemo_column_view = {
	(char *)NEMO_COLUMN_VIEW_ID,
	(char *)N_("Column View"),
	(char *)N_("_Columns"),
	(char *)N_("The column view encountered an error."),
	(char *)N_("The column view encountered an error while starting up."),
	(char *)N_("Display this location with the column view."),
	nemo_column_view_create,
	nemo_column_view_supports_uri
};

void
nemo_column_view_register (void)
{
	nemo_column_view.view_combo_label = _(nemo_column_view.view_combo_label);
	nemo_column_view.view_menu_label_with_mnemonic = _(nemo_column_view.view_menu_label_with_mnemonic);
	nemo_column_view.error_label = _(nemo_column_view.error_label);
	nemo_column_view.startup_error_label = _(nemo_column_view.startup_error_label);
	nemo_column_view.display_location_label = _(nemo_column_view.display_location_label);

	nemo_view_factory_register (&nemo_column_view);
}
