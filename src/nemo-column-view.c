/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

#include <config.h>
#include "nemo-column-view.h"

#include "nemo-application.h"
#include "nemo-error-reporting.h"
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
	COLUMN_OPACITY,
	COLUMN_NUM_COLUMNS
};

typedef struct {
	NemoColumnView *view;

	GtkWidget *column_widget;
	GtkWidget *scrolled_window;
	GtkWidget *tree_view;
	GtkWidget *empty_label;
	GtkWidget *stack;
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

	NemoColumnViewColumn *selection_column;

	gulong clipboard_info_id;

	gboolean show_hidden_files;

	NemoFileSortType sort_type;
	gboolean sort_reversed;

	gboolean click_to_rename;
	gint64 last_slow_click_time;
	GtkTreePath *last_click_path;

	GtkWidget *preview_panel;
	GtkWidget *preview_center_box;
	GtkWidget *preview_image_frame;
	GtkWidget *preview_image;
	GtkWidget *preview_text_scrolled;
	GtkWidget *preview_text_view;
	GtkWidget *preview_meta_grid;
	GtkWidget *preview_empty_label;
	NemoFile *previewed_file;
	gulong previewed_file_changed_id;
	GCancellable *preview_cancel;
};

G_DEFINE_TYPE (NemoColumnView, nemo_column_view, NEMO_TYPE_VIEW);

#define parent_class nemo_column_view_parent_class

static void column_view_column_load_directory (NemoColumnViewColumn *col, GFile *location);
static void column_view_column_free (NemoColumnViewColumn *col);
static void column_view_column_clear (NemoColumnViewColumn *col);
static void column_view_rebuild_after_column (NemoColumnView *view, gint column_index);
static void column_view_update_selection (NemoColumnView *view);
static void column_view_notify_selection_changed (NemoColumnView *view);
static void column_view_on_selection_changed (GtkTreeSelection *tree_selection, gpointer user_data);
static void column_view_clear_other_columns_selection (NemoColumnView *view, NemoColumnViewColumn *keep_col);
static void column_view_clear (NemoView *nemo_view);
static void column_view_column_files_added_cb (NemoDirectory *directory, GList *files, gpointer user_data);
static void column_view_column_update_empty_state (NemoColumnViewColumn *col);
static void column_view_resort_all (NemoColumnView *view);
static void column_view_update_sort_actions (NemoColumnView *view);
static void column_view_sort_radio_callback (GtkAction *action, GtkRadioAction *current, gpointer user_data);
static void column_view_reversed_order_callback (GtkToggleAction *action, gpointer user_data);
static void column_view_sort_order_changed_cb (GSettings *settings, gchar *key, gpointer user_data);
static void column_view_sort_reverse_changed_cb (GSettings *settings, gchar *key, gpointer user_data);
static char * column_view_get_backing_uri (NemoView *nemo_view);
static char * column_view_get_uri (NemoView *nemo_view);
static gboolean column_view_is_read_only (NemoView *nemo_view);
static void column_view_clipboard_info_cb (NemoClipboardMonitor *monitor, NemoClipboardInfo *info, NemoColumnView *view);
static void column_view_refresh_cut_state (NemoColumnView *view);
static void column_view_text_cell_edited_cb (GtkCellRendererText *cell, const char *path_str, const char *new_text, NemoColumnViewColumn *col);
static void column_view_text_cell_editing_canceled_cb (GtkCellRendererText *cell, NemoColumnViewColumn *col);
static void column_view_rename_done_cb (NemoFile *file, GFile *result_location, GError *error, gpointer user_data);
static void column_view_update_preview (NemoColumnView *view);
static void column_view_preview_clear (NemoColumnView *view);
static void column_view_preview_free_resources (NemoColumnView *view);

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
static gboolean
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

	col->empty_label = gtk_label_new ("This folder is empty");
	gtk_widget_set_halign (col->empty_label, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (col->empty_label, GTK_ALIGN_CENTER);
	gtk_style_context_add_class (gtk_widget_get_style_context (col->empty_label),
				     GTK_STYLE_CLASS_DIM_LABEL);

	{
		GtkWidget *stack;

		stack = gtk_stack_new ();
		gtk_stack_set_homogeneous (GTK_STACK (stack), TRUE);
		gtk_stack_add_named (GTK_STACK (stack), col->scrolled_window, "view");
		gtk_stack_add_named (GTK_STACK (stack), col->empty_label, "empty");
		gtk_stack_set_visible_child_name (GTK_STACK (stack), "view");
		col->stack = stack;
		gtk_box_pack_start (GTK_BOX (col->column_widget), stack, TRUE, TRUE, 0);
		gtk_widget_show (stack);
	}
	gtk_widget_show (col->scrolled_window);
	gtk_widget_show (col->empty_label);

	col->list_store = gtk_list_store_new (COLUMN_NUM_COLUMNS,
					      GDK_TYPE_PIXBUF,
					      G_TYPE_STRING,
					      G_TYPE_POINTER,
					      G_TYPE_BOOLEAN,
					      G_TYPE_DOUBLE);

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
						     "opacity", COLUMN_OPACITY,
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
						     "opacity", COLUMN_OPACITY,
						     NULL);
		gtk_tree_view_column_set_min_width (tv_column, 100);
		gtk_tree_view_column_set_resizable (tv_column, TRUE);
		gtk_tree_view_append_column (GTK_TREE_VIEW (col->tree_view), tv_column);

		g_signal_connect (col->text_renderer, "edited",
				  G_CALLBACK (column_view_text_cell_edited_cb), col);
		g_signal_connect (col->text_renderer, "editing-canceled",
				  G_CALLBACK (column_view_text_cell_editing_canceled_cb), col);
	}

	selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);

	g_signal_connect (selection, "changed",
			  G_CALLBACK (column_view_on_selection_changed), view);
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

	if (col->tree_view != NULL) {
		GtkTreeSelection *selection;

		selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
		if (selection != NULL) {
			g_signal_handlers_disconnect_by_data (selection, col->view);
		}
		g_signal_handlers_disconnect_by_data (col->tree_view, col->view);
		col->tree_view = NULL;
	}

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

	if (col->stack != NULL) {
		gtk_stack_set_visible_child_name (GTK_STACK (col->stack), "view");
	}
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
			    COLUMN_OPACITY, 1.0,
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

	column_view_column_update_empty_state (col);
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
				if (nemo_file_is_gone (changed_file)) {
					nemo_file_unref (changed_file);
					gtk_list_store_remove (col->list_store, &iter);
				} else {
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
				}
				break;
			}
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
		}
	}

	column_view_column_update_empty_state (col);
}

static void
column_view_column_update_empty_state (NemoColumnViewColumn *col)
{
	if (col == NULL || col->stack == NULL) {
		return;
	}

	if (!col->loading &&
	    gtk_tree_model_iter_n_children (GTK_TREE_MODEL (col->list_store), NULL) == 0) {
		gtk_stack_set_visible_child_name (GTK_STACK (col->stack), "empty");
	} else {
		gtk_stack_set_visible_child_name (GTK_STACK (col->stack), "view");
	}
}

static void
column_view_column_done_loading_cb (NemoDirectory *directory, gpointer user_data)
{
	NemoColumnViewColumn *col = user_data;
	col->loading = FALSE;
	column_view_column_update_empty_state (col);
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
column_view_clear_other_columns_selection (NemoColumnView *view, NemoColumnViewColumn *keep_col)
{
	GList *l;

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;

		if (col == keep_col) {
			continue;
		}

		gtk_tree_selection_unselect_all (
			gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view)));
	}
}

static void
column_view_rebuild_after_column (NemoColumnView *view, gint column_index)
{
	while (g_list_length (view->priv->columns) > column_index + 1) {
		NemoColumnViewColumn *col = g_list_last (view->priv->columns)->data;
		view->priv->columns = g_list_remove (view->priv->columns, col);
		if (view->priv->selection_column == col) {
			view->priv->selection_column = NULL;
		}
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

	if (view->priv->preview_panel != NULL) {
		gtk_box_reorder_child (GTK_BOX (view->priv->columns_container),
				       view->priv->preview_panel, -1);
	}

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
			NemoColumnViewColumn *col = NULL;
			GList *l;
			gint i;

			for (i = 0, l = view->priv->columns; l != NULL; l = l->next, i++) {
				NemoColumnViewColumn *c = l->data;
				if (c->tree_view == GTK_WIDGET (tree_view)) {
					column_index = i;
					col = c;
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
				if (col != NULL) {
					column_view_clear_other_columns_selection (view, col);
					view->priv->selection_column = col;
				}
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
	GList *l;

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		if (col->tree_view == GTK_WIDGET (tree_view)) {
			view->priv->selection_column = col;
			break;
		}
	}

	column_view_update_selection (view);
	column_view_notify_selection_changed (view);
}

static void
column_view_on_selection_changed (GtkTreeSelection *tree_selection, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);
	GtkTreeView *tree_view;
	GList *l;

	tree_view = gtk_tree_selection_get_tree_view (tree_selection);
	if (tree_view == NULL) {
		return;
	}

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		if (col->tree_view == GTK_WIDGET (tree_view)) {
			view->priv->selection_column = col;
			break;
		}
	}

	column_view_update_selection (view);
	column_view_notify_selection_changed (view);
}

static gboolean
column_view_clicked_on_text (NemoColumnViewColumn *col,
			     GtkTreeView *tree_view,
			     GdkEventButton *event)
{
	GtkTreeViewColumn *tv_column;
	gint x_col_offset, x_cell_offset, width;
	gboolean ret;

	tv_column = gtk_tree_view_get_column (tree_view, 0);
	if (tv_column == NULL) {
		return FALSE;
	}

	x_col_offset = gtk_tree_view_column_get_x_offset (tv_column);

	gtk_tree_view_column_cell_get_position (tv_column,
						GTK_CELL_RENDERER (col->text_renderer),
						&x_cell_offset, &width);

	ret = (event->x > (x_col_offset + x_cell_offset) &&
	       event->x < (x_col_offset + x_cell_offset + width)) &&
	      !gtk_tree_view_is_blank_at_pos (tree_view, event->x, event->y,
					      NULL, NULL, NULL, NULL);

	return ret;
}

static gboolean
column_view_handle_slow_two_click (NemoColumnView *view,
				   NemoColumnViewColumn *col,
				   GtkTreeView *tree_view,
				   GtkTreePath *path,
				   GdkEventButton *event)
{
	gint64 current_time;
	gint interval;
	gint double_click_interval;
	gboolean ret = FALSE;

	if (!view->priv->click_to_rename) {
		return FALSE;
	}

	g_object_get (G_OBJECT (gtk_widget_get_settings (GTK_WIDGET (view))),
		      "gtk-double-click-time", &double_click_interval,
		      NULL);

	/* Slow click interval is 800ms longer than the system double-click interval. */
	interval = double_click_interval + 800;

	current_time = g_get_monotonic_time ();

	if (view->priv->last_click_path != NULL &&
	    gtk_tree_path_compare (view->priv->last_click_path, path) == 0 &&
	    current_time - view->priv->last_slow_click_time < interval * 1000 &&
	    column_view_clicked_on_text (col, tree_view, event)) {
		ret = TRUE;
	}

	if (ret) {
		/* Consume the two-click so a third click does not rename again. */
		g_clear_pointer (&view->priv->last_click_path, gtk_tree_path_free);
		return TRUE;
	}

	/* Stash for next click. */
	view->priv->last_slow_click_time = current_time;
	g_clear_pointer (&view->priv->last_click_path, gtk_tree_path_free);
	view->priv->last_click_path = gtk_tree_path_copy (path);

	return FALSE;
}

static void
column_view_click_to_rename_mode_changed (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);

	view->priv->click_to_rename = g_settings_get_boolean (nemo_preferences,
							      NEMO_PREFERENCES_CLICK_TO_RENAME);
}

static gboolean
column_view_on_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (user_data);
	GtkTreeView *tree_view = GTK_TREE_VIEW (widget);
	NemoColumnViewColumn *col = NULL;
	GList *l;

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *c = l->data;
		if (c->tree_view == GTK_WIDGET (tree_view)) {
			col = c;
			break;
		}
	}

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

				/* Click-to-rename: second slow click on the file name text. */
				if (file != NULL &&
				    nemo_file_can_rename (file) &&
				    column_view_handle_slow_two_click (view, col, tree_view, path, event)) {
					NEMO_VIEW_CLASS (G_OBJECT_GET_CLASS (NEMO_VIEW (view)))
						->start_renaming_file (NEMO_VIEW (view), file, TRUE);
					gtk_tree_path_free (path);
					return TRUE;
				}

				if (is_dir && file != NULL) {
					gint column_index = -1;
					GList *cl;
					gint i;

					for (i = 0, cl = view->priv->columns; cl != NULL; cl = cl->next, i++) {
						NemoColumnViewColumn *c = cl->data;
						if (c->tree_view == GTK_WIDGET (tree_view)) {
							column_index = i;
							col = c;
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

						if (col != NULL) {
							GtkTreeSelection *sel;

							column_view_clear_other_columns_selection (view, col);
							view->priv->selection_column = col;

							sel = gtk_tree_view_get_selection (tree_view);
							gtk_tree_selection_unselect_all (sel);
							gtk_tree_selection_select_path (sel, path);
						}
					}

					column_view_update_selection (view);
					column_view_notify_selection_changed (view);

					gtk_tree_path_free (path);
					return TRUE;
				}

				if (col != NULL) {
					gint column_index = -1;
					GList *cl;
					gint i;

					for (i = 0, cl = view->priv->columns; cl != NULL; cl = cl->next, i++) {
						NemoColumnViewColumn *c = cl->data;
						if (c->tree_view == GTK_WIDGET (tree_view)) {
							column_index = i;
							break;
						}
					}

					if (column_index >= 0) {
						column_view_rebuild_after_column (view, column_index);
					}

					column_view_clear_other_columns_selection (view, col);
					view->priv->selection_column = col;
				}
			}

			gtk_tree_path_free (path);
		} else {
			for (l = view->priv->columns; l != NULL; l = l->next) {
				NemoColumnViewColumn *c = l->data;
				gtk_tree_selection_unselect_all (
					gtk_tree_view_get_selection (GTK_TREE_VIEW (c->tree_view)));
			}
			view->priv->selection_column = NULL;
		}

		return FALSE;
	}

	if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
		GtkTreePath *right_path;

		if (col != NULL) {
			view->priv->selection_column = col;
		}

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

			if (col != NULL) {
				column_view_clear_other_columns_selection (view, col);
			}
		} else {
			GtkTreeSelection *sel = gtk_tree_view_get_selection (tree_view);
			gtk_tree_selection_unselect_all (sel);
		}

		column_view_update_selection (view);
		column_view_do_popup_menu (view, tree_view, event);
		return TRUE;
	}

	return FALSE;
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
				column_view_column_update_empty_state (col);
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

static char *
column_view_get_backing_uri (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *last_node;

	last_node = g_list_last (view->priv->columns);
	if (last_node != NULL) {
		NemoColumnViewColumn *col = last_node->data;
		if (col->location != NULL) {
			return g_file_get_uri (col->location);
		}
	}

	return NEMO_VIEW_CLASS (parent_class)->get_backing_uri (nemo_view);
}

static char *
column_view_get_uri (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *last_node;

	last_node = g_list_last (view->priv->columns);
	if (last_node != NULL) {
		NemoColumnViewColumn *col = last_node->data;
		if (col->location != NULL) {
			return g_file_get_uri (col->location);
		}
	}

	return NEMO_VIEW_CLASS (parent_class)->get_uri (nemo_view);
}

static gboolean
column_view_is_read_only (NemoView *nemo_view)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *last_node;

	last_node = g_list_last (view->priv->columns);
	if (last_node != NULL) {
		NemoColumnViewColumn *col = last_node->data;
		if (col->directory_file != NULL) {
			NemoFile *file = col->directory_file;

			return !nemo_file_can_write (file) || nemo_file_is_in_admin (file);
		}
	}

	return NEMO_VIEW_CLASS (parent_class)->is_read_only (nemo_view);
}

static void
column_view_refresh_cut_state (NemoColumnView *view)
{
	NemoClipboardInfo *info;
	GList *l;

	info = nemo_clipboard_monitor_get_clipboard_info (nemo_clipboard_monitor_get ());

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *col = l->data;
		GtkTreeIter iter;
		gboolean valid;

		valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (col->list_store), &iter);
		while (valid) {
			NemoFile *file;
			gboolean cut = FALSE;
			gdouble opacity = 1.0;

			gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
					    COLUMN_FILE, &file, -1);

			if (file != NULL && info != NULL && info->cut) {
				GList *fl;

				for (fl = info->files; fl != NULL; fl = fl->next) {
					if (NEMO_FILE (fl->data) == file) {
						cut = TRUE;
						break;
					}
				}
			}

			if (cut) {
				opacity = 0.4;
			}

			gtk_list_store_set (col->list_store, &iter, COLUMN_OPACITY, opacity, -1);

			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (col->list_store), &iter);
		}
	}
}

static void
column_view_clipboard_info_cb (NemoClipboardMonitor *monitor,
			       NemoClipboardInfo *info,
			       NemoColumnView *view)
{
	column_view_refresh_cut_state (view);
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

	view->priv->selection_column = NULL;

	column_view_preview_clear (view);

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

	if (view->priv->selection_column != NULL &&
	    g_list_find (view->priv->columns, view->priv->selection_column)) {
		col = view->priv->selection_column;
	} else {
		last_node = g_list_last (view->priv->columns);
		if (last_node == NULL) return NULL;
		col = last_node->data;
	}

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

static char *
column_view_preview_format_time (time_t t)
{
	struct tm local_tm;
	struct tm local_now;
	time_t now;
	char buf[128];

	now = time (NULL);
	localtime_r (&t, &local_tm);
	localtime_r (&now, &local_now);

	if (local_tm.tm_year == local_now.tm_year) {
		strftime (buf, sizeof (buf), "%e %b %H:%M", &local_tm);
	} else {
		strftime (buf, sizeof (buf), "%e %b %Y", &local_tm);
	}

	return g_strdup (buf);
}

static GtkWidget *
column_view_preview_meta_row (GtkGrid *grid, gint row, const char *label)
{
	GtkWidget *lbl;
	GtkWidget *val;

	lbl = gtk_label_new (label);
	gtk_widget_set_halign (lbl, GTK_ALIGN_START);
	gtk_widget_set_valign (lbl, GTK_ALIGN_START);
	gtk_style_context_add_class (gtk_widget_get_style_context (lbl),
				     GTK_STYLE_CLASS_DIM_LABEL);
	gtk_grid_attach (grid, lbl, 0, row, 1, 1);
	gtk_widget_show (lbl);

	val = gtk_label_new (NULL);
	gtk_widget_set_halign (val, GTK_ALIGN_START);
	gtk_widget_set_valign (val, GTK_ALIGN_START);
	gtk_label_set_selectable (GTK_LABEL (val), TRUE);
	gtk_label_set_xalign (GTK_LABEL (val), 0.0);
	gtk_label_set_line_wrap (GTK_LABEL (val), TRUE);
	gtk_label_set_ellipsize (GTK_LABEL (val), PANGO_ELLIPSIZE_END);
	gtk_grid_attach (grid, val, 1, row, 1, 1);
	gtk_widget_show (val);

	return val;
}

static void
column_view_preview_set_text (NemoColumnView *view, const char *text)
{
	GtkTextBuffer *buffer;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view->priv->preview_text_view));
	gtk_text_buffer_set_text (buffer, text != NULL ? text : "", -1);
	gtk_widget_show (view->priv->preview_text_scrolled);
	gtk_widget_hide (view->priv->preview_image_frame);
}

static void
column_view_preview_set_pixbuf (NemoColumnView *view, GdkPixbuf *pixbuf)
{
	GdkPixbuf *scaled;
	gint w, h;
	gdouble scale;

	if (pixbuf == NULL) {
		gtk_image_clear (GTK_IMAGE (view->priv->preview_image));
		gtk_widget_show (view->priv->preview_image_frame);
		gtk_widget_hide (view->priv->preview_text_scrolled);
		return;
	}

	w = gdk_pixbuf_get_width (pixbuf);
	h = gdk_pixbuf_get_height (pixbuf);

	scale = MIN ((gdouble) 512 / w, (gdouble) 384 / h);
	if (scale < 1.0) {
		scaled = gdk_pixbuf_scale_simple (pixbuf,
						  MAX (1, (gint) (w * scale)),
						  MAX (1, (gint) (h * scale)),
						  GDK_INTERP_BILINEAR);
	} else {
		scaled = g_object_ref (pixbuf);
	}

	gtk_image_set_from_pixbuf (GTK_IMAGE (view->priv->preview_image), scaled);
	g_object_unref (scaled);

	gtk_widget_show (view->priv->preview_image_frame);
	gtk_widget_hide (view->priv->preview_text_scrolled);
}

static void
column_view_preview_file_changed_cb (NemoFile *file, NemoColumnView *view)
{
	column_view_update_preview (view);
}

static gboolean
column_view_preview_read_more_cb (const gchar *file_contents,
				  goffset      file_size,
				  gpointer     callback_data)
{
	return (file_size < 10240);
}

typedef struct {
	NemoColumnView *view;
	NemoFile *file;
} PreviewLoadData;

static void
column_view_preview_text_loaded_cb (GObject      *source_object,
				    GAsyncResult *res,
				    gpointer      user_data)
{
	PreviewLoadData *data = user_data;
	NemoColumnView *view = data->view;
	GFile *file = G_FILE (source_object);
	char *contents = NULL;
	gsize length = 0;
	GError *error = NULL;

	if (view->priv->preview_cancel == NULL ||
	    g_cancellable_is_cancelled (view->priv->preview_cancel) ||
	    view->priv->previewed_file != data->file) {
		goto out;
	}

	if (g_file_load_partial_contents_finish (file, res,
						  &contents, &length,
						  NULL, &error)) {
		if (g_utf8_validate (contents, length, NULL)) {
			column_view_preview_set_text (view, contents);
		} else {
			column_view_preview_set_text (view, NULL);
		}
		g_free (contents);
	} else {
		if (error != NULL) {
			g_error_free (error);
		}
		column_view_preview_set_text (view, NULL);
	}

out:
	nemo_file_unref (data->file);
	g_object_unref (view);
	g_free (data);
}

static void
column_view_preview_load_text (NemoColumnView *view, NemoFile *file)
{
	GFile *location;
	char *path;
	GFile *f;
	GFileReadMoreCallback cb;
	PreviewLoadData *data;

	if (view->priv->preview_cancel != NULL) {
		g_cancellable_cancel (view->priv->preview_cancel);
		g_object_unref (view->priv->preview_cancel);
		view->priv->preview_cancel = NULL;
	}

	location = nemo_file_get_location (file);
	path = g_file_get_path (location);
	if (path == NULL) {
		g_object_unref (location);
		column_view_preview_set_text (view, NULL);
		return;
	}

	f = g_file_new_for_path (path);
	g_free (path);
	g_object_unref (location);

	view->priv->preview_cancel = g_cancellable_new ();
	cb = column_view_preview_read_more_cb;

	data = g_new0 (PreviewLoadData, 1);
	data->view = g_object_ref (view);
	data->file = nemo_file_ref (file);

	g_file_load_partial_contents_async (f, view->priv->preview_cancel, cb,
					    column_view_preview_text_loaded_cb, data);
	g_object_unref (f);
}

static void
column_view_preview_refresh (NemoColumnView *view)
{
	NemoFile *file;
	char *name;
	char *type;
	char *mime_type;
	char *time_str;
	GdkPixbuf *icon;
	guint dir_count;
	gboolean count_unreadable;
	gboolean is_dir;
	gint row;
	GtkWidget *val;
	goffset size;

	if (view->priv->preview_panel == NULL ||
	    view->priv->previewed_file == NULL) {
		return;
	}

	while (gtk_grid_get_child_at (GTK_GRID (view->priv->preview_meta_grid), 0, 0) != NULL) {
		gtk_grid_remove_row (GTK_GRID (view->priv->preview_meta_grid), 0);
	}

	file = view->priv->previewed_file;
	is_dir = (nemo_file_get_file_type (file) == G_FILE_TYPE_DIRECTORY);

	gtk_widget_hide (view->priv->preview_empty_label);
	gtk_widget_hide (view->priv->preview_image_frame);
	gtk_widget_hide (view->priv->preview_text_scrolled);

	if (is_dir) {
		icon = nemo_file_get_icon_pixbuf (file, 256, TRUE, 1,
						  NEMO_FILE_ICON_FLAGS_NONE);
		column_view_preview_set_pixbuf (view, icon);
		if (icon != NULL) {
			g_object_unref (icon);
		}

		if (nemo_file_get_directory_item_count (file, &dir_count, &count_unreadable) &&
		    dir_count == 0 && !count_unreadable) {
			gtk_widget_show (view->priv->preview_empty_label);
		}
	} else {
		mime_type = nemo_file_get_mime_type (file);

		nemo_file_set_load_deferred_attrs (file, NEMO_FILE_LOAD_DEFERRED_ATTRS_YES);

		if (mime_type != NULL &&
		    (g_str_has_prefix (mime_type, "image/") ||
		     g_str_has_prefix (mime_type, "video/"))) {
			icon = nemo_file_get_icon_pixbuf (file, 512, TRUE, 1,
							  NEMO_FILE_ICON_FLAGS_USE_THUMBNAILS);
			column_view_preview_set_pixbuf (view, icon);
			if (icon != NULL) {
				g_object_unref (icon);
			}
		} else if (mime_type != NULL &&
			   (g_str_has_prefix (mime_type, "text/") ||
			    g_str_equal (mime_type, "application/x-python") ||
			    g_str_equal (mime_type, "application/json") ||
			    g_str_equal (mime_type, "application/xml") ||
			    g_str_equal (mime_type, "application/x-shellscript"))) {
			if (nemo_file_is_local (file)) {
				column_view_preview_load_text (view, file);
			} else {
				column_view_preview_set_text (view, NULL);
			}
		} else {
			icon = nemo_file_get_icon_pixbuf (file, 256, TRUE, 1,
							  NEMO_FILE_ICON_FLAGS_NONE);
			column_view_preview_set_pixbuf (view, icon);
			if (icon != NULL) {
				g_object_unref (icon);
			}
		}

		g_free (mime_type);
	}

	name = nemo_file_get_display_name (file);
	type = nemo_file_get_string_attribute (file, "detailed_type");
	if (type == NULL || type[0] == '\0') {
		g_free (type);
		type = nemo_file_get_type_as_string (file);
	}

	row = 0;
	val = column_view_preview_meta_row (GTK_GRID (view->priv->preview_meta_grid), row++, "Name:");
	gtk_label_set_text (GTK_LABEL (val), name);

	val = column_view_preview_meta_row (GTK_GRID (view->priv->preview_meta_grid), row++, "Type:");
	gtk_label_set_text (GTK_LABEL (val), type);

	size = nemo_file_get_size (file);
	if (size > 0) {
		char *size_str;
		val = column_view_preview_meta_row (GTK_GRID (view->priv->preview_meta_grid), row++, "Size:");
		size_str = g_format_size_full (size, G_FORMAT_SIZE_LONG_FORMAT);
		gtk_label_set_text (GTK_LABEL (val), size_str);
		g_free (size_str);
	}

	if (nemo_file_get_mtime (file) > 0) {
		val = column_view_preview_meta_row (GTK_GRID (view->priv->preview_meta_grid), row++, "Modified:");
		time_str = column_view_preview_format_time (nemo_file_get_mtime (file));
		gtk_label_set_text (GTK_LABEL (val), time_str);
		g_free (time_str);
	}

	if (nemo_file_get_ctime (file) > 0) {
		val = column_view_preview_meta_row (GTK_GRID (view->priv->preview_meta_grid), row++, "Created:");
		time_str = column_view_preview_format_time (nemo_file_get_ctime (file));
		gtk_label_set_text (GTK_LABEL (val), time_str);
		g_free (time_str);
	}

	{
		GFile *parent;
		char *parent_str;

		parent = nemo_file_get_parent_location (file);
		if (parent != NULL) {
			parent_str = g_file_get_parse_name (parent);
			val = column_view_preview_meta_row (GTK_GRID (view->priv->preview_meta_grid), row++, "Location:");
			gtk_label_set_text (GTK_LABEL (val), parent_str);
			g_free (parent_str);
			g_object_unref (parent);
		}
	}

	g_free (name);
	g_free (type);
}

static void
column_view_preview_free_resources (NemoColumnView *view)
{
	if (view->priv->previewed_file != NULL) {
		if (view->priv->previewed_file_changed_id > 0) {
			g_signal_handler_disconnect (view->priv->previewed_file,
						     view->priv->previewed_file_changed_id);
			view->priv->previewed_file_changed_id = 0;
		}
		nemo_file_unref (view->priv->previewed_file);
		view->priv->previewed_file = NULL;
	}

	if (view->priv->preview_cancel != NULL) {
		g_cancellable_cancel (view->priv->preview_cancel);
		g_object_unref (view->priv->preview_cancel);
		view->priv->preview_cancel = NULL;
	}
}

static void
column_view_preview_clear (NemoColumnView *view)
{
	column_view_preview_free_resources (view);

	if (view->priv->preview_panel != NULL) {
		gtk_widget_hide (view->priv->preview_panel);
		gtk_widget_hide (view->priv->preview_image_frame);
		gtk_widget_hide (view->priv->preview_text_scrolled);
		gtk_widget_hide (view->priv->preview_empty_label);
		while (gtk_grid_get_child_at (GTK_GRID (view->priv->preview_meta_grid), 0, 0) != NULL) {
			gtk_grid_remove_row (GTK_GRID (view->priv->preview_meta_grid), 0);
		}
	}
}

static void
column_view_update_preview (NemoColumnView *view)
{
	GList *selection;
	NemoFile *file;

	if (view->priv->preview_panel == NULL) {
		return;
	}

	selection = column_view_get_selection (NEMO_VIEW (view));
	if (selection == NULL || selection->next != NULL) {
		if (selection != NULL) {
			nemo_file_list_free (selection);
		}
		column_view_preview_clear (view);
		return;
	}

	file = NEMO_FILE (selection->data);
	nemo_file_list_free (selection);

	if (view->priv->preview_panel != NULL) {
		gtk_widget_show (view->priv->preview_panel);
	}

	if (view->priv->previewed_file == file) {
		column_view_preview_refresh (view);
		return;
	}

	column_view_preview_clear (view);

	view->priv->previewed_file = nemo_file_ref (file);
	view->priv->previewed_file_changed_id =
		g_signal_connect (view->priv->previewed_file, "changed",
				  G_CALLBACK (column_view_preview_file_changed_cb), view);

	column_view_preview_refresh (view);
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

	column_view_update_preview (view);
}

static void
column_view_notify_selection_changed (NemoColumnView *view)
{
	nemo_view_notify_selection_changed (NEMO_VIEW (view));
	g_signal_emit_by_name (view, "selection-changed", 0);
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
	NemoColumnViewColumn *col = view->priv->selection_column;

	if (col == NULL || !g_list_find (view->priv->columns, col)) {
		col = column_view_get_column_at_index (view, 0);
	}

	if (col != NULL) {
		GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (col->tree_view));
		gtk_tree_selection_select_all (sel);
	}
	column_view_update_selection (view);
	column_view_notify_selection_changed (view);
}

static void
column_view_set_selection (NemoView *nemo_view, GList *selection)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *l;
	NemoColumnViewColumn *col;

	col = view->priv->selection_column;
	if (col == NULL || !g_list_find (view->priv->columns, col)) {
		col = column_view_get_column_at_index (view, 0);
	}
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
	column_view_notify_selection_changed (view);
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
column_view_rename_done_cb (NemoFile *file,
			    GFile *result_location,
			    GError *error,
			    gpointer user_data)
{
	if (error != NULL && error->code != G_IO_ERROR_CANCELLED) {
		nemo_report_error_renaming_file (file, NULL, error, NULL);
	}
}

static void
column_view_text_cell_editing_canceled_cb (GtkCellRendererText *cell,
					   NemoColumnViewColumn *col)
{
	g_object_set (G_OBJECT (cell), "editable", FALSE, NULL);
	nemo_view_unfreeze_updates (NEMO_VIEW (col->view));
}

static void
column_view_text_cell_edited_cb (GtkCellRendererText *cell,
				 const char *path_str,
				 const char *new_text,
				 NemoColumnViewColumn *col)
{
	GtkTreePath *path;
	GtkTreeIter iter;
	NemoFile *file = NULL;
	gboolean valid;

	path = gtk_tree_path_new_from_string (path_str);
	valid = gtk_tree_model_get_iter (GTK_TREE_MODEL (col->list_store), &iter, path);
	if (valid) {
		gtk_tree_model_get (GTK_TREE_MODEL (col->list_store), &iter,
				    COLUMN_FILE, &file, -1);
	}
	gtk_tree_path_free (path);

	if (file != NULL && new_text[0] != '\0') {
		gchar *name = nemo_file_get_display_name (file);

		if (strcmp (new_text, name) != 0) {
			nemo_rename_file (file, new_text, column_view_rename_done_cb, NULL);
		}

		g_free (name);
		nemo_file_unref (file);
	}

	g_object_set (G_OBJECT (cell), "editable", FALSE, NULL);
	nemo_view_unfreeze_updates (NEMO_VIEW (col->view));
}

static void
column_view_start_renaming_file (NemoView *nemo_view, NemoFile *file, gboolean select_all)
{
	NemoColumnView *view = NEMO_COLUMN_VIEW (nemo_view);
	GList *l;
	GtkTreeViewColumn *tv_column;
	GtkTreePath *path;
	NemoColumnViewColumn *col = NULL;
	GtkTreeIter iter;
	gboolean valid;

	for (l = view->priv->columns; l != NULL; l = l->next) {
		NemoColumnViewColumn *c = l->data;
		NemoFile *existing;

		valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (c->list_store), &iter);
		while (valid) {
			gtk_tree_model_get (GTK_TREE_MODEL (c->list_store), &iter,
					    COLUMN_FILE, &existing, -1);
			if (existing == file) {
				col = c;
				break;
			}
			valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (c->list_store), &iter);
		}
		if (col != NULL) {
			break;
		}
	}

	if (col == NULL) {
		return;
	}

	g_object_set (G_OBJECT (col->text_renderer), "editable", TRUE, NULL);

	nemo_view_freeze_updates (nemo_view);

	path = gtk_tree_model_get_path (GTK_TREE_MODEL (col->list_store), &iter);
	tv_column = gtk_tree_view_get_column (GTK_TREE_VIEW (col->tree_view), 0);

	gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (col->tree_view),
				      path, tv_column, TRUE, 0.0, 0.0);
	gtk_tree_view_set_cursor_on_cell (GTK_TREE_VIEW (col->tree_view),
					  path, tv_column,
					  GTK_CELL_RENDERER (col->text_renderer),
					  TRUE);

	gtk_tree_path_free (path);
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
			view->priv->selection_column = col;
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
					view->priv->selection_column = col;
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

	{
		GtkWidget *main_hbox;
		GtkWidget *columns_scroller;
		GtkWidget *separator;

		main_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_container_add (GTK_CONTAINER (view), main_hbox);
		gtk_widget_show (main_hbox);

		columns_scroller = gtk_scrolled_window_new (NULL, NULL);
		gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (columns_scroller),
						GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
		gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (columns_scroller),
						     GTK_SHADOW_NONE);
		gtk_box_pack_start (GTK_BOX (main_hbox), columns_scroller, TRUE, TRUE, 0);
		gtk_container_add (GTK_CONTAINER (columns_scroller),
				   view->priv->columns_container);
		gtk_widget_show (columns_scroller);
		gtk_widget_show (view->priv->columns_container);

		view->priv->preview_panel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
		gtk_widget_set_size_request (view->priv->preview_panel, 600, -1);

		separator = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
		gtk_box_pack_start (GTK_BOX (view->priv->preview_panel), separator,
				    FALSE, TRUE, 0);
		gtk_widget_show (separator);

		view->priv->preview_center_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		gtk_box_pack_start (GTK_BOX (view->priv->preview_panel),
				    view->priv->preview_center_box, TRUE, TRUE, 0);
		gtk_widget_show (view->priv->preview_center_box);

		view->priv->preview_image_frame = gtk_frame_new (NULL);
		gtk_frame_set_shadow_type (GTK_FRAME (view->priv->preview_image_frame),
					   GTK_SHADOW_IN);
		gtk_widget_set_size_request (view->priv->preview_image_frame, -1, 400);
		gtk_widget_set_valign (view->priv->preview_image_frame, GTK_ALIGN_CENTER);
		gtk_widget_set_halign (view->priv->preview_image_frame, GTK_ALIGN_CENTER);

		view->priv->preview_image = gtk_image_new ();
		gtk_container_add (GTK_CONTAINER (view->priv->preview_image_frame),
				   view->priv->preview_image);
		gtk_widget_show (view->priv->preview_image);

		gtk_box_pack_start (GTK_BOX (view->priv->preview_center_box),
				    view->priv->preview_image_frame, TRUE, FALSE, 0);
		gtk_widget_show (view->priv->preview_image_frame);

		view->priv->preview_text_scrolled = gtk_scrolled_window_new (NULL, NULL);
		gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (view->priv->preview_text_scrolled),
						GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtk_widget_set_size_request (view->priv->preview_text_scrolled, -1, 400);

		view->priv->preview_text_view = gtk_text_view_new ();
		gtk_text_view_set_editable (GTK_TEXT_VIEW (view->priv->preview_text_view), FALSE);
		gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (view->priv->preview_text_view), FALSE);
		gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (view->priv->preview_text_view),
					     GTK_WRAP_WORD);
		gtk_container_add (GTK_CONTAINER (view->priv->preview_text_scrolled),
				   view->priv->preview_text_view);
		gtk_widget_show (view->priv->preview_text_view);

		gtk_box_pack_start (GTK_BOX (view->priv->preview_center_box),
				    view->priv->preview_text_scrolled, TRUE, TRUE, 0);
		gtk_widget_show (view->priv->preview_text_scrolled);

		view->priv->preview_empty_label = gtk_label_new ("This folder is empty");
		gtk_widget_set_halign (view->priv->preview_empty_label, GTK_ALIGN_START);
		gtk_style_context_add_class (gtk_widget_get_style_context (view->priv->preview_empty_label),
					     GTK_STYLE_CLASS_DIM_LABEL);
		gtk_box_pack_start (GTK_BOX (view->priv->preview_panel),
				    view->priv->preview_empty_label, FALSE, FALSE, 0);
		gtk_widget_hide (view->priv->preview_empty_label);

		view->priv->preview_meta_grid = gtk_grid_new ();
		gtk_grid_set_row_spacing (GTK_GRID (view->priv->preview_meta_grid), 4);
		gtk_grid_set_column_spacing (GTK_GRID (view->priv->preview_meta_grid), 12);
		gtk_widget_set_halign (view->priv->preview_meta_grid, GTK_ALIGN_START);
		gtk_widget_set_valign (view->priv->preview_meta_grid, GTK_ALIGN_START);
		gtk_box_pack_end (GTK_BOX (view->priv->preview_panel),
				  view->priv->preview_meta_grid, FALSE, FALSE, 0);
		gtk_widget_show (view->priv->preview_meta_grid);

		gtk_box_pack_start (GTK_BOX (view->priv->columns_container),
				    view->priv->preview_panel, FALSE, TRUE, 0);
		gtk_widget_hide (view->priv->preview_panel);
	}

	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (view),
					GTK_POLICY_NEVER, GTK_POLICY_NEVER);

	view->priv->zoom_level = NEMO_ZOOM_LEVEL_STANDARD;
	view->priv->current_selection = NULL;
	view->priv->current_selection_count = -1;
	view->priv->selection_column = NULL;
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

	column_view_click_to_rename_mode_changed (NEMO_VIEW (view));

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

	view->priv->clipboard_info_id =
		g_signal_connect (nemo_clipboard_monitor_get (),
				  "clipboard_info",
				  G_CALLBACK (column_view_clipboard_info_cb), view);
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

	if (view->priv->clipboard_info_id > 0) {
		g_signal_handler_disconnect (nemo_clipboard_monitor_get (),
					     view->priv->clipboard_info_id);
		view->priv->clipboard_info_id = 0;
	}

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

	column_view_preview_free_resources (view);

	g_clear_pointer (&view->priv->last_click_path, gtk_tree_path_free);

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
	nemo_view_class->get_backing_uri = column_view_get_backing_uri;
	nemo_view_class->get_uri = column_view_get_uri;
	nemo_view_class->is_read_only = column_view_is_read_only;
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
	nemo_view_class->click_to_rename_mode_changed = column_view_click_to_rename_mode_changed;
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
