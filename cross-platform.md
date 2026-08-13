# Cross-Platform: Column View'i Windows/macOS'a Taşıma Planı

> Bu doküman, Nemo'daki macOS Finder tarzı Column View (Miller Columns) özelliğini
> Windows ve macOS'ta çalışacak ayrı bir uygulamaya taşıma planını kaydeder.
> Şu an uygulanmıyor; ileride düşünülmek üzere not olarak tutulur.
> Tarih: 2026-08-02

## Amaç

Nemo'daki Column View mantığını, Windows/macOS/Linux'ta çalışan **yeni bir Qt 6
(C++ backend + QML UI)** uygulamasına taşımak. Ayrı bir proje olarak başlayacak
(mevcut Nemo repoyla bağlantısız).

## Değerlendirme Özeti

- **Nemo'yu bütün olarak Windows/macOS'a taşımak pratik değil.** Temeli
  neredeyse tamamen Linux/GNOME altyapısına dayanıyor:
  - `gio-unix-2.0`, `x11`, `X11/XF86keysym.h` → doğrudan X11/Unix API
  - `cinnamon-desktop`, `xapp` → Cinnamon'a özgü
  - `sys/mount.h`, `sys/vfs.h` → Linux mount sistemi
  - GSettings/dconf, GVFS (trash:, network:, computer:), GDesktopAppInfo
    (.desktop), MIME veritabanı
  - Masaüstü entegrasyonu (`nemo-desktop`, desktop icons) — Windows/mac'ta anlamsız
- **macOS'ta zaten bu özellik var**: Finder, 1995'ten beri Miller Columns
  kullanıyor. macOS kullanıcısı için mevcut özelliğin taklidi olur.
- **Windows için değerli**: Windows Explorer'da kolon görünümü yok (topluluk
  projeleri olan Files vb. bunu ekliyor).

## Mimari Haritalama (Nemo → Qt)

| Nemo konsepti | Qt/QML karşılığı |
|---|---|
| `NemoColumnViewColumn` (GtkVBox+scrolled+treeview) | QML `ColumnPanel` = `Flickable` + `ListView`, `Row` içinde |
| `NemoDirectory` + `nemo_directory_get_file_list()` | `QDir`/`QFileInfo` + QtConcurrent ile **async** listing |
| `nemo_directory_file_monitor_add` + "files_added/changed" | `QFileSystemWatcher` + debounce (kolon başına) |
| `nemo_file_get_icon_pixbuf` | `QFileIconProvider` + önbellek (QML `Image`/`Icon` için) |
| `column_view_rebuild_after_column()` | QML `columnModel`: `index`'ten sonraki öğeleri sil, yenisini ekle |
| `column_view_on_row_activated` / button_press | QML `ListView` + `MouseArea` / `Keys` (çift tık, Enter) |
| Sıralama (`nemo_file_compare_for_sort`) | C++ `QSortFilterProxyModel` alt sınıfı (dizinler-önce, tür, boyut, mtime) |
| Preview panel (image/video/text/metadata) | QML `previewPane`: `Image`, `MediaPlayer`, `Text`, metadata grid |
| Address bar / location-changed | QML `PathBar` + `location` state |

## 1. C++ Backend (`src/` — QtCore/QtGui)

- **`fileentry.h/cpp`**: `QFileInfo` sarmalayıcı: ad, tam yol, tür, boyut, mtime,
  isDir, isSymlink, MIME tipi (`QMimeDatabase`)
- **`dirfetcher.h/cpp`**: `QFuture`+`QThreadPool` ile async `QDirIterator`;
  sinyaller: `listingReady(QUrl, QList<FileEntry>)`, `listingFailed`
- **`dirwatcher.h/cpp`**: `QFileSystemWatcher` + 300ms debounce timer;
  `dirChanged`/`fileChanged` → yeniden listele
- **`fileiconprovider.h/cpp`**: `QFileIconProvider` tabanlı; `QCache` ile ikon
  önbelleği; thumbnail'ler için `QImageReader` boyut kısıtı
- **`sortproxy.h/cpp`**: `QSortFilterProxyModel` alt sınıfı — sıralama anahtarı +
  ters + "dizinler önce" mantığı (Nemo `nemo_file_compare_for_sort` senkronu)
- **`session.h/cpp`**: `QSettings` ile pencere boyutu, son konum, sort tercihleri

## 2. Column View Motoru (`src/columnview.h/cpp`)

- `ColumnViewEngine` sınıfı: `QList<Column>` state; `navigateTo(dir)`,
  `rebuildAfter(index)`, `back()`, `forward()` — Nemo'nun
  `column_view_rebuild_after_column` / `column_view_update_address_bar` mantığının
  birebir portu
- Her kolon bir `Column` struct: `QUrl dir`, `QAbstractListModel*` (dolu model),
  `FileSystemWatcher*`
- `currentSelection()` → seçili dosya; sinyal `selectionChanged(QUrl)` → preview

## 3. QML UI (`qml/`)

- `main.qml`: `ApplicationWindow` + `Row { Repeater { columnModel } }` + sağda
  preview pane
- `ColumnPanel.qml`: başlık (dizin adı), `ListView` (dizin/sayı sıralaması,
  ikon + metin), seçim/kaydırma
- `PreviewPane.qml`: `Loader` — görsel→`Image` (thumbnail), video→
  `MediaPlayer`+`VideoOutput`, metin→`Text`, diğer→ikon + metadata `GridLayout`
- `PathBar.qml`: tıklanabilir breadcrumb; `Tab` ile kolonlar arası geçiş;
  geri/ileri butonları

## 4. Platform Katmanı

- **Windows**: `trash:` benzeri GVFS yok — `QUrl.fromLocalFile` ile tam yol;
  çöp kutusu opsiyonel (Win32 `SHFileOperation` wrapper, v1 sonrası)
- **macOS**: `~/Desktop`, `~/Documents` gibi kullanıcı dizinleri `QStandardPaths`;
  çöp kutusu opsiyonel
- **Linux**: tam yerel dosya sistemi (GVFS gerektirmez); Nemo ile aynı görünüm

## 5. Build & Dağıtım

- **Qt 6.x** (>=6.5), CMake; `find_package(Qt6 COMPONENTS Core Gui Quick
  QuickControls2 Multimedia)`
- CMake + `qml6` modülleri; `qt_add_executable` + `qt_add_qml_module`
- **Windows**: MinGW/MSVC bundle (windeployqt) → MSI/NSIS; GitHub Actions
  `windows-latest`
- **macOS**: `macdeployqt` → DMG, ad hoc veya developer ID imzalı; `macos-14`
  runner
- **Linux**: AppImage/deb opsiyonel (zaten Nemo var)

## 6. Faz Planı

1. **Faz 0**: Boş Qt6/QML projesi iskeleti + CMake + pencere açılır
2. **Faz 1**: Backend (FileEntry, DirFetcher async, icon provider) + tek kolonlu
   ListView (ev dizini)
3. **Faz 2**: Kolon motoru — tıklayınca yeni kolon, `rebuildAfter`, kolon silme,
   sıralama (Linux'ta doğrula)
4. **Faz 3**: Preview pane (görsel/metin/metadata), thumbnail önbelleği
5. **Faz 4**: PathBar, geri/ileri, klavye gezinme (Tab/ok), ayarlar kalıcılığı
6. **Faz 5**: Windows derlemesi (windeployqt) + GitHub Actions + kurulum paketi
7. **Faz 6**: macOS derlemesi (macdeployqt) + DMG

## 7. Riskler

- **Thumbnail/önizleme tutarlılığı**: Nemo'nun `nemo_file_get_icon_pixbuf`
  (büyük ikon + metin + thumbnail fallback) portu en zorlu kısım; Qt thumbnail
  desteği sınırlı → `QImageReader` + MIME'e göre custom generate
- **Async + watcher tutarlılığı**: Hızlı gezinmede kolon silinirken geri dönen
  `QFuture` → lifetime yönetimi (kolon başına lifecycle guard)
- **Çöp kutusu / ağ sürücüleri**: GVFS olmadan kısıtlı — v1'de sadece yerel FS,
  sonra ağ paylaşımları (`\\server` / `smb://`) opsiyonel
- **Klon hakları**: Linux Mint Nemo (GPL-2/3) — ayrı proje kod içermiyorsa temiz;
  isim "Nemo" olamaz (marka/karışıklık), örn. "Kolon" / "Miller Browser"

## Not

Proje şu an **uygulanmıyor**. Bu dosya, ileride bu yöne gidilirse izlenecek
yol haritasını saklamak içindir.
