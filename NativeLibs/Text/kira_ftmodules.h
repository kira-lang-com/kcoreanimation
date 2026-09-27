/*
 * Kira text engine — trimmed FreeType module registry.
 *
 * FreeType's default `ftmodule.h` registers every bundled driver (type1, cid,
 * pfr, t42, winfnt, pcf, bdf, sdf, svg, ...). We only vendor and compile the
 * subset needed for high-quality scalable UI text, so `ftinit.c` must only
 * reference modules that are actually linked — otherwise the build fails with
 * undefined driver symbols.
 *
 * Compiled module set (see Text.toml `[build] sources`):
 *   - autofit      : auto-hinter
 *   - truetype     : TrueType / OpenType-glyf driver
 *   - cff          : CFF / OpenType-CFF driver
 *   - psaux        : Type1/CFF charstring interpreter (CFF dependency)
 *   - psnames      : glyph-name <-> code mapping (CFF/SFNT dependency)
 *   - pshinter     : PostScript hinter (CFF dependency)
 *   - sfnt         : shared SFNT table loader (TrueType/CFF dependency)
 *   - smooth       : anti-aliased coverage rasterizer (8-bit alpha)
 *   - raster1      : monochrome rasterizer (fallback)
 *
 * This keeps coverage for the overwhelmingly common UI font formats (.ttf,
 * .otf, .ttc) while avoiding legacy bitmap/Type1 drivers we do not ship.
 */

FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )

/* EOF */
