# Qt Advanced Docking System (vendored)

- Upstream: https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System
- Version: tag v5.1.1 (released 2026-08-20)
- Archive: https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/archive/refs/tags/v5.1.1.tar.gz
- Archive SHA-256: 9ce947acbb1984946f9145c976e42e6984f99b98fb414dd7354a290af6d4ff04
- Downloaded: 2026-10-11, with the user's explicit approval
- Licence: LGPL-2.1 (`LICENSE`, `gnu-lgpl-v2.1.md`). Built as a separate shared library and linked dynamically, so users can replace it (D-026, D-153).
- Kept: `src/`, `cmake/`, top-level `CMakeLists.txt`, `adsConfig.cmake`, licence files and README. Dropped: `demo/`, `doc/`, `examples/`, `sip/`, `persist/`, Python packaging.
- Modified: the two source files under "Esses changes" below. Every other file is byte-identical to its copy in the archive, and no CMake file was changed. Each changed file says so below its licence header and at each change, with the date (LGPL-2.1 section 2(b)); the changes are under the library's licence. `third_party/README.md` has the tree digest before and after them.

## Esses changes

Both fix undefined behaviour that the soak's UndefinedBehaviorSanitizer reports (`.github/workflows/bench.yml`, Clang 18, `halt_on_error=1`) and that a local Clang 17 build with `-fsanitize=undefined` reproduces in `widgets.tst_dock_host`. Upstream `master` still has both (checked on 2026-10-25 at 087b35a of 2026-10-23), and no upstream issue or pull request reports them. Drop a change once an upstream release fixes it and the vendored copy is updated to that release. The diffs below show the code with spaces and the comments shortened; the files indent with tabs.

### 1. `src/AutoHideSideBar.cpp`, `CAutoHideSideBar::insertDockWidget()` (2026-10-25)

The dock manager creates its `CDockFocusController` only when the `FocusHighlighting` config flag is set at its construction; Esses leaves the flag off (`libs/widgets/src/shell/ads_backend.cpp`). Every other use of the controller in ADS checks the pointer (`DockManager.cpp`) or the flag (`DockWidgetTab.cpp`, `DockAreaTitleBar.cpp`; null there only if the flag were set after the manager was created, which Esses never does). `insertDockWidget()`, through which every auto-hide pin goes (`CDockWidget::setAutoHide()`, the dock area's pin button, a drop on a side bar, `restoreState()` of a saved side bar), called `clearDockWidgetFocus()` on the null pointer: "runtime error: member call on null pointer of type 'ads::CDockFocusController'" at `AutoHideSideBar.cpp:217:52`. Without the flag the skipped call did nothing visible: the dock widget never holds the focus itself (its focus policy stays `Qt::NoFocus`), and the `focused` property it set on the dock widget and its tab is read by no stylesheet without focus highlighting. Upstream code: https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/blob/master/src/AutoHideSideBar.cpp

```diff
     auto AutoHideContainer = new CAutoHideDockContainer(DockWidget, d->SideTabArea, d->ContainerWidget);
-    DockWidget->dockManager()->dockFocusController()->clearDockWidgetFocus(DockWidget);
+    // Esses change, 2026-10-25: (four comment lines)
+    auto FocusController = DockWidget->dockManager()->dockFocusController();
+    if (FocusController)
+    {
+        FocusController->clearDockWidgetFocus(DockWidget);
+    }
     auto Tab = AutoHideContainer->autoHideTab();
```

### 2. `src/DockManager.cpp`, `CDockManager::~CDockManager()` (2026-10-25)

`CDockManager` is its own dock container, constructed with itself as that container's manager. After `~CDockManager()` had finished, `~CDockContainerWidget()` read the container's `QPointer<CDockManager>` back-pointer to that same object (`QPointer::data()` is a `static_cast` from `QObject`) and called `removeDockContainer()` on it: "runtime error: downcast of address ... which does not point to an object of type 'ads::CDockManager'" (`qpointer.h:74`, from `DockContainerWidget.cpp:1527`), on every destruction of a dock manager. `~CDockManager()` now clears the back-pointer with the container's own `removeFromDockManager()` while the object is still a `CDockManager`. `removeDockContainer()` ignores the manager itself, so the call that no longer happens did nothing else. Upstream code: https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/blob/master/src/DockContainerWidget.cpp

```diff
     for ( auto area : areas )
     {
         delete area;
     }
+    // Esses change, 2026-10-25: (six comment lines)
+    removeFromDockManager();
     delete d;
 }
```
