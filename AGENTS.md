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

## Future Features (To Be Added)

- [x] Right-click context menu in columns
- [x] Fix "Open in Terminal" to use correct deep directory
- [ ] Column resizing (drag column borders)
- [ ] Back/forward navigation buttons sync
- [x] Drag & drop between columns
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

> **Ortam notu:** Bu geliştirme makinesinde **sudo yetkisi yok**. Paket (`*.deb`)
> üretimi ve kurulumu root gerektirmeden, `DESTDIR` + `dpkg-deb --root-owner-group`
> ile yapılır (bkz. `.github/workflows/package.yml`). `meson install` her zaman
> `DESTDIR=/tmp/staging` ile çalıştır; sistem dizinlerine (`/usr`) doğrudan yazma.
> Deb'i yerelde kurmak için `sudo` yerine makine sahibinden/CI'dan yararlan.

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
