# Nemo Column View - Development Guide

> This document describes how to add a macOS Finder-style Column View (Miller Columns) to the Nemo file manager. It captures the complete development process, build instructions, common pitfalls, and files modified.

---

## Quick Start

```bash
# Clone and build
git clone https://github.com/linuxmint/nemo.git
cd nemo

# Apply all changes listed in this document, then:
meson setup build
ninja -C build

# Run (schemas must be compiled to build/schemas/)
mkdir -p build/schemas
cp libnemo-private/org.nemo.gschema.xml build/schemas/
glib-compile-schemas build/schemas/
GSETTINGS_SCHEMA_DIR=$(pwd)/build/schemas ./build/src/nemo
```

---

## Files Created

| File | Purpose |
|------|---------|
| `src/nemo-column-view.h` | GObject type definitions, `NEMO_COLUMN_VIEW_ID` |
| `src/nemo-column-view.c` | Full implementation (~1250 lines) |

---

## Files Modified

### Build System
| File | Change |
|------|--------|
| `src/meson.build` | Add `nemo-column-view.c` to sources |
| `meson.build` | Bump version if needed |

### Application Entry Point
| File | Change |
|------|--------|
| `src/nemo-main-application.c` | Add `#include "nemo-column-view.h"` and `nemo_column_view_register()` call |
| `src/nemo-application.c` | Add `#include "nemo-column-view.h"` |

### View Switching (Toolbar & Menu)
| File | Change |
|------|--------|
| `src/nemo-actions.h` | `#define NEMO_ACTION_COLUMN_VIEW "ColumnView"` |
| `src/nemo-window-menus.c` | Add `COLUMN_VIEW=4` to view enum; add entry to `view_radio_entries[]`; add `action_column_view_callback`; update `action_for_view_id()`, `toolbar_set_view_button()`, `menu_set_view_selection()` |
| `src/nemo-toolbar.c` | Add `column_view_button` to priv struct; create button with "Columns" label; bind visibility to `show-column-view-icon-toolbar` GSettings key |
| `gresources/nemo-shell-ui.xml` | Add `<menuitem name="Column View" action="ColumnView"/>` |

### Preferences & Schema
| File | Change |
|------|--------|
| `libnemo-private/nemo-global-preferences.h` | Add `NEMO_PREFERENCES_SHOW_COLUMN_VIEW_ICON_TOOLBAR` key |
| `libnemo-private/org.nemo.gschema.xml` | Add `show-column-view-icon-toolbar` key; add `column-view` to `FolderView` enum |
| `libnemo-private/nemo-global-preferences.c` | Map `NEMO_DEFAULT_FOLDER_VIEWER_OTHER` to `"OAFIID:Nemo_File_Manager_Column_View"` |
| `src/nemo-file-management-properties.c` | Add `"column-view"` to `default_view_values[]` |
| `gresources/nemo-file-management-properties.glade` | Add "Column View" row to `model1` list store |

### Nemo View ID
| File | Change |
|------|--------|
| `src/nemo-icon-view.h` | Add `#define NEMO_COLUMN_VIEW_ID "OAFIID:Nemo_File_Manager_Column_View"` |

---

## Implementation Architecture

### Key Structs

```c
// Column widget state
typedef struct {
    NemoColumnView *view;
    GtkWidget *column_widget;       // GtkVBox
    GtkWidget *scrolled_window;
    GtkWidget *tree_view;
    GtkListStore *list_store;       // columns: ICON(GdkPixbuf), NAME(string), FILE(pointer), IS_DIR(boolean)
    GtkCellRendererPixbuf *icon_renderer;
    GtkCellRendererText *text_renderer;
    NemoDirectory *directory;       // NemoDirectory for this column
    NemoFile *directory_file;       // NemoFile representing the directory
    GFile *location;                // GFile for this column's location
    gulong files_added_id;          // signal handler: "files_added"
    gulong files_changed_id;        // signal handler: "files_changed"
    gulong done_loading_id;         // signal handler: "done_loading"
    gpointer monitor_client;        // key for nemo_directory_file_monitor_*
    gboolean loading;
} NemoColumnViewColumn;

// Main view private data
struct _NemoColumnViewPriv {
    GtkWidget *columns_container;   // GtkHBox holding all columns
    GList *columns;                 // GList of NemoColumnViewColumn*
    NemoZoomLevel zoom_level;
    GList *current_selection;       // Cached selection
    gint current_selection_count;
    gboolean show_hidden_files;
};
```

### Data Flow

1. **View creation**: `nemo_column_view_init()` creates `columns_container` (GtkHBox)
2. **Loading**: `column_view_begin_loading()` (NemoView virtual called via `finish_loading` → `BEGIN_LOADING` signal)
   - Clears all columns, appends one new column
   - Calls `column_view_column_load_directory()` which:
     - Creates `NemoDirectory` via `nemo_directory_get()`
     - Gets initial files via `nemo_directory_get_file_list()`
     - Sets up `nemo_directory_file_monitor_add()` for ongoing updates
     - Connects "files_added"/"files_changed"/"done_loading" signals
3. **File rendering**: `column_view_column_add_file()` refs the NemoFile, creates icon via `nemo_file_get_icon_pixbuf(size, TRUE, 1, NEMO_FILE_ICON_FLAGS_NONE)`, stores in GtkListStore
4. **Navigation**: `column_view_on_button_press()` / `column_view_on_row_activated()`
   - Gets the clicked file's GFile location
   - Calls `column_view_rebuild_after_column()` to destroy columns to the right
   - Appends new column, loads directory
   - Updates address bar via `column_view_update_address_bar()`

### Virtual Method Overrides

```
NemoViewClass:
    add_file           → column_view_add_file
    remove_file        → column_view_remove_file
    file_changed       → column_view_file_changed
    begin_loading      → column_view_begin_loading
    end_loading        → column_view_end_loading
    clear              → column_view_clear
    get_selection      → column_view_get_selection
    get_selection_count→ column_view_get_selection_count
    get_selection_for_file_transfer → column_view_get_selection
    get_item_count     → column_view_get_item_count
    is_empty           → column_view_is_empty
    end_file_changes   → column_view_end_file_changes
    select_all         → column_view_select_all
    set_selection      → column_view_set_selection
    invert_selection   → column_view_invert_selection
    bump_zoom_level    → column_view_bump_zoom_level
    zoom_to_level      → column_view_zoom_to_level
    get_zoom_level     → column_view_get_zoom_level
    restore_default_zoom_level → column_view_restore_default_zoom_level
    get_default_zoom_level → column_view_get_default_zoom_level
    can_zoom_in        → column_view_can_zoom_in
    can_zoom_out       → column_view_can_zoom_out
```

---

## Critical Gotchas & Lessons Learned

### 1. NemoFile Reference Counting (MOST IMPORTANT)

**Problem**: GtkListStore stores NemoFile pointers as `G_TYPE_POINTER` (raw pointers, no ref counting). When Nemo's directory model frees files, the list store has dangling pointers → SIGSEGV.

**Solution**: Always `nemo_file_ref()` when adding to the list store and `nemo_file_unref()` when removing/clearing.

```c
// In column_view_column_add_file:
nemo_file_ref(file);
gtk_list_store_set(col->list_store, &iter, COLUMN_FILE, file, ...);

// In column_view_column_clear:
gtk_tree_model_get(..., COLUMN_FILE, &file, -1);
if (file != NULL) nemo_file_unref(file);
gtk_list_store_clear(col->list_store);
```

**In `column_view_column_remove_file`:**
```c
if (existing_file == file) {
    nemo_file_unref(file);  // BEFORE removing
    gtk_list_store_remove(col->list_store, &iter);
    return;
}
```

### 2. Selection List Reference Counting

**Problem**: `nemo_view_display_selection_info()` calls `nemo_file_list_free(selection)` which calls `nemo_file_unref()` on every file in the list. Our `column_view_get_selection()` returned raw pointers → double unref → crash.

**Solution**: Call `nemo_file_list_ref()` before returning from `get_selection()`. Use `nemo_file_list_free()` when replacing cached selection.

```c
// In column_view_get_selection:
selection = g_list_reverse(selection);
nemo_file_list_ref(selection);  // CRITICAL
return selection;

// In column_view_update_selection:
if (view->priv->current_selection != NULL) {
    nemo_file_list_free(view->priv->current_selection);  // NOT g_list_free
}
view->priv->current_selection = column_view_get_selection(nemo_view);
```

### 3. Cleanup Order in column_view_column_free

**Order matters**:
1. `column_view_column_clear(col)` — unref files BEFORE destroying list store
2. `gtk_widget_destroy(col->column_widget)` — destroy widget
3. Cancel callbacks: `nemo_directory_cancel_callback()`
4. Disconnect ALL signals: `files_added_id`, `files_changed_id`, `done_loading_id`
5. `nemo_directory_file_monitor_remove()`
6. `nemo_directory_unref(col->directory)`
7. `nemo_file_unref(col->directory_file)`
8. `g_object_unref(col->location)`
9. `g_free(col)`

**Never** call `gtk_container_remove()` before `gtk_widget_destroy()` — the container remove can finalize the widget, then `gtk_widget_destroy()` operates on freed memory.

### 4. File Monitoring Setup

**Problem**: Files don't appear in subdirectory columns if only `nemo_directory_file_monitor_add()` is used. This function monitors for CHANGES, not for the initial file list.

**Solution**: Use THREE layers:
```c
// Layer 1: immediate file list (for cached directories)
GList *fl = nemo_directory_get_file_list(col->directory);
if (fl != NULL) {
    column_view_column_files_added_cb(col->directory, fl, col);
    nemo_file_list_free(fl);
}

// Layer 2: when-ready callback (for uncached directories)
nemo_directory_call_when_ready(col->directory, attributes, FALSE,
                                column_view_column_files_added_cb, col);

// Layer 3: ongoing monitoring (for file changes)
nemo_directory_file_monitor_add(col->directory, &col->directory,
                                 show_hidden, attributes,
                                 column_view_column_files_added_cb, col);

// Layer 4: signals (required for NemoDirectory file discovery)
col->files_added_id = g_signal_connect(col->directory, "files_added",
                                        G_CALLBACK(column_view_column_files_added_cb), col);
col->files_changed_id = g_signal_connect(col->directory, "files_changed",
                                          G_CALLBACK(column_view_column_files_changed_cb), col);
```

### 5. Deduplication

Both our manual loading and NemoView's monitoring can add the same files. Add dedup in `column_view_column_add_file`:

```c
// Check if file already exists in list store
valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(col->list_store), &existing);
while (valid) {
    gtk_tree_model_get(..., COLUMN_FILE, &existing_file, -1);
    if (existing_file == file) return FALSE;  // already exists
    valid = gtk_tree_model_iter_next(...);
}
// Dedup check BEFORE nemo_file_ref() to avoid double-ref
nemo_file_ref(file);
```

### 6. NemoView's Own Monitoring

**Problem**: NemoView's `finish_loading()` sets up `nemo_directory_file_monitor_add(view->details->model, &view->details->model, ...)` for the MAIN model. If we also add monitoring for the same directory with key `&col->directory`, files get added TWICE.

**Solution**: Keep our deduplication. For the first column (loaded via `begin_loading`), NemoView's monitoring + our monitoring both fire, but dedup prevents duplicates. For additional columns, only our monitoring fires.

### 7. Address Bar Update

**Problem**: Column navigation bypasses NemoView's `load_location` → `load_directory` flow, so the address bar shows the old location.

**Solution**: After column navigation, manually update `slot->location`:
```c
static void
column_view_update_address_bar(NemoColumnView *view, GFile *location) {
    NemoWindowSlot *slot = nemo_view_get_nemo_window_slot(NEMO_VIEW(view));
    if (slot == NULL || location == NULL) return;
    GFile *old_location = slot->location;
    slot->location = g_object_ref(location);
    if (old_location) g_object_unref(old_location);
    g_signal_emit_by_name(slot, "location-changed", from_uri, to_uri);
    nemo_window_pane_sync_location_widgets(slot->pane);
}
```
Need `#include "nemo-window-pane.h"`.

### 8. `column_view_finalize` - Don't Destroy Widgets

**Problem**: Calling `gtk_widget_destroy()` in `finalize()` operates on already-destroyed widgets.

**Solution**: GTK destroys widgets automatically during the widget destruction chain. In `finalize()`, only clean up non-widget resources:
- Disconnect GSettings signal handler
- For each column: disconnect signals, remove monitors, unref directories/files/locations, `g_free(col)`
- Free column list

### 9. Row-Activated (Double-Click) on Directories

**Problem**: Using `nemo_view_activate_file()` for directories triggers NemoView's full `load_location` → `load_directory` → `begin_loading` flow, which calls `column_view_clear()` and destroys all columns.

**Solution**: Handle directory activation the same as single-click (append new column):
```c
if (nemo_file_is_directory(file)) {
    // same as button_press: append column, load directory, update address bar
} else {
    nemo_file_ref(file);
    nemo_view_activate_file(NEMO_VIEW(view), file, 0);  // flag 0, not CLOSE_BEHIND
    nemo_file_unref(file);
}
```

### 10. Icon Rendering

**Problem**: Using `gtk_cell_renderer_pixbuf_new()` with `"icon-name"` attribute causes crash with scale factor issues on HiDPI.

**Solution**: Use `GDK_TYPE_PIXBUF` column type and `nemo_file_get_icon_pixbuf()` with scale=1:
```c
icon = nemo_file_get_icon_pixbuf(file,
    nemo_get_icon_size_for_zoom_level(col->view->priv->zoom_level),
    TRUE, 1, NEMO_FILE_ICON_FLAGS_NONE);
gtk_list_store_set(col->list_store, &iter, COLUMN_ICON, icon, ...);
g_object_unref(icon);  // list_store holds its own ref
```

### 11. Right-click Context Menu

**Problem**: Right-click always showed the selection context menu, even when clicking empty space. This was because the widget-level `button_press_event` handler intercepted all right-clicks and always called `nemo_view_pop_up_selection_context_menu()`.

**Solution**: Added `column_view_do_popup_menu()` helper that checks the clicked tree_view's selection state:
- If the tree_view has selected rows → `nemo_view_pop_up_selection_context_menu()` (Open, Cut, Copy, Properties, etc.)
- If no selection → `nemo_view_pop_up_background_context_menu()` (New Folder, Paste, Properties, etc.)

Three handlers modified:

1. **`column_view_on_button_press`** (per-tree-view): On right-click, checks if click is on a row. If on a non-selected row → selects it. If on blank space → deselects all. Then calls `column_view_do_popup_menu`.

2. **`column_view_button_press_event`** (widget-level): Uses `gtk_get_event_widget()` to find which tree_view was clicked. Falls back to background menu if no tree_view found.

3. **`column_view_popup_menu`** (keyboard Shift+F10): Finds the focused column via `gtk_widget_has_focus(col->tree_view)`. Falls back to background menu if no column has focus.

**Important**: The right-click on a non-selected row should SELECT that row first (so the menu operates on the correct file). This matches the list view's behavior.

```c
if (gtk_tree_view_get_path_at_pos(tree_view, x, y, &path, NULL, NULL, NULL)) {
    if (!gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
    }
    gtk_tree_path_free(path);
} else {
    gtk_tree_selection_unselect_all(sel);
}
```

### 12. Background Menu "Open in Terminal" Shows Wrong Directory

**Problem**: Right-click background → "Open in Terminal" opens in the INITIAL directory (e.g., `/home/ulas`), not the currently-viewed deep directory (e.g., `/home/ulas/Documents/project/lib`).

**Root cause**: `action_open_in_terminal_callback()` in `nemo-view.c:7378` calls `nemo_view_get_uri(view)` which reads from `view->details->model`. Our column navigation updates `slot->location` (for the address bar) but cannot update `view->details->model` because `NemoViewDetails` is a private struct defined in `nemo-view.c`.

**Workaround**: Select a folder first, then right-click → "Open in Terminal". When a folder is selected, the callback uses `nemo_file_get_path(selected_file)` instead, which gives the correct directory.

**Potential fix**: Add a public function `nemo_view_update_model(NemoView *view, NemoDirectory *dir)` that updates `view->details->model`. Or override the background popup path handling. This needs more investigation.

### 13. GSettings Signal Connection

**Problem**: `g_signal_connect_swapped` causes parameter misalignment. The "changed" signal on GSettings has signature `(GSettings *settings, gchar *key, gpointer user_data)`. With swapped, the callback receives `(user_data, settings, key)`, breaking the callback's parameter expectations.

**Solution**: Use `g_signal_connect` (NOT `_swapped`) for GSettings "changed" signals. The user_data parameter will be the view pointer.

```c
// CORRECT:
g_signal_connect (nemo_preferences,
                  "changed::" NEMO_PREFERENCES_SHOW_HIDDEN_FILES,
                  G_CALLBACK (column_view_hidden_files_changed_cb),
                  view);

// WRONG (causes invalid cast crash):
g_signal_connect_swapped (nemo_preferences, ...);
```

---

## Future Features (To Be Added)

- [x] Right-click context menu in columns
- [ ] Fix "Open in Terminal" to use correct deep directory
- [ ] Column resizing (drag column borders)
- [ ] Back/forward navigation buttons sync
- [ ] Drag & drop between columns
- [ ] Keyboard navigation: Tab between columns, arrow keys within column
- [ ] Thumbnail previews in columns
- [ ] Column header with directory name
- [ ] Scroll position preservation when navigating
- [ ] File selection across multiple columns
- [ ] Filter/search within column
- [ ] New folder/file from context menu in column
- [ ] Column view as default in desktop preferences
- [ ] Split pane (F3) support within column view

---

## Build & Test

```bash
# Build
meson setup build
ninja -C build

# Schema setup
mkdir -p build/schemas
cp libnemo-private/org.nemo.gschema.xml build/schemas/
glib-compile-schemas build/schemas/

# Run
GSETTINGS_SCHEMA_DIR=$(pwd)/build/schemas ./build/src/nemo

# Debug with gdb
GSETTINGS_SCHEMA_DIR=$(pwd)/build/schemas gdb ./build/src/nemo
```

## Version

```bash
# In meson.build root:
version: '6.5.0',
```

## Sürüm Yükseltme (Release) Süreci

> Her yeni sürümde şu adımları takip et. `master`'a push edildiğinde `.github/workflows/package.yml` otomatik olarak `.deb` üretir; `v*` etiketi push edilince GitHub Release oluşturulur.

### 1. Sürümü Yükselt

Sürümü **tek kaynaktan** değiştir (workflow sürümü buradan okur):

- `meson.build` → `project('nemo', 'c', version : 'X.Y.Z', ...)`
- `debian/changelog` → başına yeni kayıt ekle: `nemo (X.Y.Z) unstable; urgency=medium` + değişiklik listesi (kod tarzı: `[ Column View ]` altında `* ...` maddeleri)

### 2. Sürüm Alırken Dikkat Edilecekler

- **Kolon sıralama ayarı** (`default-sort-order` GSettings anahtarı) `org.nemo.SortOrder` enum'ındaki nick'lerle sınırlıdır: `name`, `size`, `type`, `detailed_type`, `mtime`, `atime`, `trash-time`. Yeni bir sıralama seçeneği eklenirse hem enum'a hem `column_view_sort_map` tablosuna hem de `gresources/nemo-column-view-ui.xml`'e eklenmeli.
- **GSettings anahtarı eklendiyse**: `org.nemo.gschema.xml` + `nemo-global-preferences.h` birlikte güncellenmeli. `glib-compile-schemas --strict` ile doğrula.
- **UI dosyası eklendiyse**: `gresources/nemo.gresource.xml`'e `<file>` satırı eklenmeli, aksi halde gresource'da derlenmez.
- **`nemo-file.c` sıralama karşılaştırıcısı**: `nemo_file_compare_for_sort()` dizinler-önce / favoriler-önce / ters sırayı kendisi halleder; kolon görünümünde `sort_reversed`'i ayrıca `GTK_SORT_DESCENDING` ile çift uygulama (çift ters çevirme).
- **İkon boyutu** zoom'a bağlıdır: `column_view_refresh_icons()` her zoom değişiminde tüm kolonlardaki `COLUMN_ICON`'u yeniden üretir. Zoom ile ilgili değişiklik yaparken bu fonksiyonun `column_view_zoom_to_level` ve `column_view_bump_zoom_level` sonunda çağrıldığından emin ol.
- **`.deb` bağımlılıkları**: CI'da `dpkg-shlibdeps`, kendi kütüphanemizi (`libnemo-extension.so.1`) `shlibs.local` + `-l` flag'leriyle çözer. Bu mekanizmayı bozma; yeni bir özel kütüphane eklenirse aynı yöntemle `-l`'e eklenmeli.
- **Derleme**: `ninja -C build` hatasız tamamlanmalı (exit 0). CI'da `gtk_layer_shell=true` olduğu için Wayland bağımlılıkları (libgtk-layer-shell0, libwayland-client0) pakete girer.

### 3. Release Notlarını Yaz

- `vX.Y.Z` etiketi oluştur ve push et:
  ```bash
  git tag vX.Y.Z
  git push simple-nemo vX.Y.Z
  ```
- Workflow `.deb`'i üretir ve **GitHub Release oluşturur**, ancak release **description'ını otomatik doldurmaz**.
- **Release metni İNGİLİZCE yazılmalıdır** (kullanıcıya dönük, GitHub üzerinden okunacak). değişiklik listesi için `debian/changelog`'daki İngilizce kayıtları esas al.
- Bu yüzden: sürümde neler değiştiğini (kullanıcı görünürü değişiklikler) **"Release description" alanına elle yaz** ve yayınla. Kullanıcıya dönük değişiklikler önceliklidir (yeni özellik, bugfix, davranış değişikliği). Kurulum komutunu da eklemek faydalı olur:
  ```bash
  sudo dpkg -i nemo_X.Y.Z_amd64.deb
  ```
