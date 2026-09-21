# devOS Modular App Drop-in Template

This directory provides the canonical starter template for developing and registering modular applications in **`devOS`**.

---

## 1. Architectural Principles

According to **AGENTS.md Directives (Rule #8)**:
* All applications must implement the standardized `devos_app_descriptor_t` interface.
* Applications register at boot time via `devos_core_register_app()`.
* **Zero Modification Rule:** Adding a new application must **never** require modifying the Home Screen (`app_launcher.c`) or hardcoding app IDs into closed enums.
* Dual-Core Task Pinning: Pin UI handling and LVGL manipulation to **Core 1**, and network/crypto/file operations to **Core 0**.

---

## 2. Descriptor Interface

Every app must expose a function returning its `devos_app_descriptor_t`:

```c
typedef struct {
    devos_app_id_t id;          /* Auto-assigned or unique enum */
    const char *uid;            /* Stable string identifier, e.g. "my_app" */
    const char *name;           /* Short tile name for launcher cards, e.g. "MyApp" */
    const char *title;          /* Full application title */
    const char *subtitle;       /* Descriptive subtitle */
    const char *icon;           /* LVGL symbol string, e.g. LV_SYMBOL_EDIT */
    const char *category;       /* Category identifier: "tools", "system", "agents", etc. */
    lv_obj_t *screen;           /* Root LVGL screen container */
    void (*init)(void);         /* Lifecycle: called once at boot */
    void (*show)(void);         /* Lifecycle: called when app becomes active */
    void (*hide)(void);         /* Lifecycle: called when navigating away */
    bool (*handle_key)(uint32_t key, uint8_t modifiers); /* Physical keyboard input */
    int (*get_telemetry_lines)(char lines[3][64]);        /* Dynamic launcher tile lines */
} devos_app_descriptor_t;
```

---

## 3. Step-by-Step Integration Guide

### Step 1: Create App Directory
Create a folder under `main/apps/app_<name>/`:
```bash
mkdir -p main/apps/app_myapp
```

### Step 2: Implement Descriptor and Handlers
In `app_myapp.c`:
```c
#include "app_myapp.h"
#include "devos_theme.h"

static devos_app_descriptor_t s_desc;
static lv_obj_t *s_screen = NULL;

static void myapp_init(void) {
    const devos_palette_t *p = devos_theme_get();
    s_screen = lv_obj_create(lv_screen_active());
    s_desc.screen = s_screen;
    lv_obj_set_size(s_screen, DEVOS_SCREEN_WIDTH, DEVOS_CONTENT_HEIGHT);
    lv_obj_set_pos(s_screen, 0, DEVOS_TOP_BAR_HEIGHT);
    lv_obj_set_style_bg_color(s_screen, p->bg, 0);
    // ... build UI ...
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
}

static int myapp_telemetry(char lines[3][64]) {
    snprintf(lines[0], 64, "* Status: Active");
    snprintf(lines[1], 64, "* Value: 42");
    snprintf(lines[2], 64, "* Port: 8080");
    return 3;
}

devos_app_descriptor_t *app_myapp_get_descriptor(void) {
    s_desc.uid = "myapp";
    s_desc.name = "MyApp";
    s_desc.title = "My Modular Application";
    s_desc.subtitle = "Demo utility";
    s_desc.icon = LV_SYMBOL_BELL;
    s_desc.category = "tools";
    s_desc.init = myapp_init;
    s_desc.get_telemetry_lines = myapp_telemetry;
    return &s_desc;
}
```

### Step 3: Register in `main/main.c`
In `devos_system_bringup()`:
```c
#include "app_myapp.h"
// ...
devos_core_register_app(app_myapp_get_descriptor());
```

### Step 4: Add to `CMakeLists.txt`
Add include directory and source file:
```cmake
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/main/apps/app_myapp)
set(DEVOS_SOURCES
    ...
    main/apps/app_myapp/app_myapp.c
)
```

The launcher will automatically discover the app, allocate a compact 280×210 tile, display its icon and telemetry, and support arrangement, hotkeys, and pagination!
