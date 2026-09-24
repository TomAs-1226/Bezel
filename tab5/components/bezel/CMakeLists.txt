idf_component_register(SRC_DIRS "src" "fonts" INCLUDE_DIRS "include" REQUIRES heap
                       # whole: LVGL (linked after this) calls the allocator in src/bz_lv_mem.c
                       WHOLE_ARCHIVE)
