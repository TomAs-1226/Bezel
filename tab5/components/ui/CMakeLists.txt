# unbuilt/ holds the ruler, storage and flashlight apps, left out of the build ON PURPOSE as redundant (settings and files
# cover the card; the others did a phone's job); each file says how to bring its app back.
# Only src/ is compiled. fonts_c/ holds home mode's four face fonts as C arrays (tools/make_face_fonts.py) and is left
# out ON PURPOSE: 760 KB of glyph bitmaps in the image are 760 KB of PSRAM taken for good, because the program runs
# from PSRAM (CONFIG_SPIRAM_XIP_FROM_PSRAM). The firmware loads them from the card instead (src/ui_face_fonts.c);
# to bake one back in, move its .c into src/ and bind the face to it in place of the card's.
idf_component_register(SRC_DIRS "src" INCLUDE_DIRS "include" REQUIRES bezel catalyst nt4 tab_hal assist home intel LDFRAGMENTS "linker.lf")
