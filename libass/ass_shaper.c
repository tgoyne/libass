/*
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <fribidi/fribidi.h>

#include "ass_shaper.h"
#include "ass_render.h"
#include "ass_font.h"
#include "ass_parse.h"
#include "ass_cache.h"

#define MAX_RUNS 50

#ifdef CONFIG_HARFBUZZ
#include <harfbuzz/hb-ft.h>
enum {
    VERT = 0,
    VKNA,
    KERN
};
#define NUM_FEATURES 3
#endif

struct ass_shaper {
    ASS_ShapingLevel shaping_level;

    // FriBidi log2vis
    int n_glyphs;
    FriBidiChar *event_text;
    FriBidiCharType *ctypes;
    FriBidiLevel *emblevels;
    FriBidiStrIndex *cmap;
    FriBidiParType base_direction;

#ifdef CONFIG_HARFBUZZ
    // OpenType features
    int n_features;
    hb_feature_t *features;
    hb_language_t language;

    // Glyph metrics cache, to speed up shaping
    Cache *metrics_cache;
#endif
};

#ifdef CONFIG_HARFBUZZ
struct ass_shaper_metrics_data {
    Cache *metrics_cache;
    GlyphMetricsHashKey hash_key;
    int vertical;
};

struct ass_shaper_font_data {
    hb_font_t *fonts[ASS_FONT_MAX_FACES];
    hb_font_funcs_t *font_funcs[ASS_FONT_MAX_FACES];
    struct ass_shaper_metrics_data *metrics_data[ASS_FONT_MAX_FACES];
};
#endif

/**
 * \brief Print version information
 */
void ass_shaper_info(ASS_Library *lib)
{
    ass_msg(lib, MSGL_V, "Shaper: FriBidi "
            FRIBIDI_VERSION " (SIMPLE)"
#ifdef CONFIG_HARFBUZZ
            " HarfBuzz-ng %s (COMPLEX)", hb_version_string()
#endif
           );
}

/**
 * \brief grow arrays, if needed
 * \param new_size requested size
 */
static void check_allocations(ASS_Shaper *shaper, size_t new_size)
{
    if (new_size > shaper->n_glyphs) {
        shaper->event_text = realloc(shaper->event_text, sizeof(FriBidiChar) * new_size);
        shaper->ctypes     = realloc(shaper->ctypes, sizeof(FriBidiCharType) * new_size);
        shaper->emblevels  = realloc(shaper->emblevels, sizeof(FriBidiLevel) * new_size);
        shaper->cmap       = realloc(shaper->cmap, sizeof(FriBidiStrIndex) * new_size);
    }
}

/**
 * \brief Free shaper and related data
 */
void ass_shaper_free(ASS_Shaper *shaper)
{
#ifdef CONFIG_HARFBUZZ
    ass_cache_done(shaper->metrics_cache);
    free(shaper->features);
#endif
    free(shaper->event_text);
    free(shaper->ctypes);
    free(shaper->emblevels);
    free(shaper->cmap);
    free(shaper);
}

void ass_shaper_font_data_free(ASS_ShaperFontData *priv)
{
#ifdef CONFIG_HARFBUZZ
    int i;
    for (i = 0; i < ASS_FONT_MAX_FACES; i++)
        if (priv->fonts[i]) {
            free(priv->metrics_data[i]);
            hb_font_destroy(priv->fonts[i]);
            hb_font_funcs_destroy(priv->font_funcs[i]);
        }
    free(priv);
#endif
}

#ifdef CONFIG_HARFBUZZ
/**
 * \brief set up the HarfBuzz OpenType feature list with some
 * standard features.
 */
static void init_features(ASS_Shaper *shaper)
{
    shaper->features = calloc(sizeof(hb_feature_t), NUM_FEATURES);

    shaper->n_features = NUM_FEATURES;
    shaper->features[VERT].tag = HB_TAG('v', 'e', 'r', 't');
    shaper->features[VERT].end = INT_MAX;
    shaper->features[VKNA].tag = HB_TAG('v', 'k', 'n', 'a');
    shaper->features[VKNA].end = INT_MAX;
    shaper->features[KERN].tag = HB_TAG('k', 'e', 'r', 'n');
    shaper->features[KERN].end = INT_MAX;
}

/**
 * \brief Set features depending on properties of the run
 */
static void set_run_features(ASS_Shaper *shaper, GlyphInfo *info)
{
        // enable vertical substitutions for @font runs
        if (info->font->desc.vertical)
            shaper->features[VERT].value = shaper->features[VKNA].value = 1;
        else
            shaper->features[VERT].value = shaper->features[VKNA].value = 0;
}

/**
 * \brief Update HarfBuzz's idea of font metrics
 * \param hb_font HarfBuzz font
 * \param face associated FreeType font face
 */
static void update_hb_size(hb_font_t *hb_font, FT_Face face)
{
    hb_font_set_scale (hb_font,
            ((uint64_t) face->size->metrics.x_scale * (uint64_t) face->units_per_EM) >> 16,
            ((uint64_t) face->size->metrics.y_scale * (uint64_t) face->units_per_EM) >> 16);
    hb_font_set_ppem (hb_font, face->size->metrics.x_ppem,
            face->size->metrics.y_ppem);
}


/*
 * Cached glyph metrics getters follow
 *
 * These functions replace HarfBuzz' standard FreeType font functions
 * and provide cached access to essential glyph metrics. This usually
 * speeds up shaping a lot. It also allows us to use custom load flags.
 *
 */

GlyphMetricsHashValue *
get_cached_metrics(struct ass_shaper_metrics_data *metrics, FT_Face face,
                   hb_codepoint_t glyph)
{
    GlyphMetricsHashValue *val;

    metrics->hash_key.glyph_index = glyph;
    val = ass_cache_get(metrics->metrics_cache, &metrics->hash_key);

    if (!val) {
        int load_flags = FT_LOAD_DEFAULT | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH
            | FT_LOAD_IGNORE_TRANSFORM;
        GlyphMetricsHashValue new_val;

        if (FT_Load_Glyph(face, glyph, load_flags))
            return NULL;

        memcpy(&new_val.metrics, &face->glyph->metrics, sizeof(FT_Glyph_Metrics));

        // if @font rendering is enabled and the glyph should be rotated,
        // make cached_h_advance pick up the right advance later
        if (metrics->vertical && glyph >= VERTICAL_LOWER_BOUND)
            new_val.metrics.horiAdvance = new_val.metrics.vertAdvance;

        val = ass_cache_put(metrics->metrics_cache, &metrics->hash_key, &new_val);
    }

    return val;
}

static hb_bool_t
get_glyph(hb_font_t *font, void *font_data, hb_codepoint_t unicode,
          hb_codepoint_t variation, hb_codepoint_t *glyph, void *user_data)
{
    FT_Face face = font_data;

    if (variation)
        *glyph = FT_Face_GetCharVariantIndex(face, unicode, variation);
    else
        *glyph = FT_Get_Char_Index(face, unicode);

    return *glyph != 0;
}

static hb_position_t
cached_h_advance(hb_font_t *font, void *font_data, hb_codepoint_t glyph,
                 void *user_data)
{
    FT_Face face = font_data;
    struct ass_shaper_metrics_data *metrics_priv = user_data;
    GlyphMetricsHashValue *metrics = get_cached_metrics(metrics_priv, face, glyph);

    if (!metrics)
        return 0;

    return metrics->metrics.horiAdvance;
}

static hb_position_t
cached_v_advance(hb_font_t *font, void *font_data, hb_codepoint_t glyph,
                 void *user_data)
{
    FT_Face face = font_data;
    struct ass_shaper_metrics_data *metrics_priv = user_data;
    GlyphMetricsHashValue *metrics = get_cached_metrics(metrics_priv, face, glyph);

    if (!metrics)
        return 0;

    return metrics->metrics.vertAdvance;

}

static hb_bool_t
cached_h_origin(hb_font_t *font, void *font_data, hb_codepoint_t glyph,
                hb_position_t *x, hb_position_t *y, void *user_data)
{
    return 1;
}

static hb_bool_t
cached_v_origin(hb_font_t *font, void *font_data, hb_codepoint_t glyph,
                hb_position_t *x, hb_position_t *y, void *user_data)
{
    FT_Face face = font_data;
    struct ass_shaper_metrics_data *metrics_priv = user_data;
    GlyphMetricsHashValue *metrics = get_cached_metrics(metrics_priv, face, glyph);

    if (!metrics)
        return 0;

    *x = metrics->metrics.horiBearingX - metrics->metrics.vertBearingX;
    *y = metrics->metrics.horiBearingY - (-metrics->metrics.vertBearingY);

    return 1;
}

static hb_position_t
get_h_kerning(hb_font_t *font, void *font_data, hb_codepoint_t first,
                 hb_codepoint_t second, void *user_data)
{
    FT_Face face = font_data;
    FT_Vector kern;

    if (FT_Get_Kerning (face, first, second, FT_KERNING_DEFAULT, &kern))
        return 0;

    return kern.x;
}

static hb_position_t
get_v_kerning(hb_font_t *font, void *font_data, hb_codepoint_t first,
                 hb_codepoint_t second, void *user_data)
{
    return 0;
}

static hb_bool_t
cached_extents(hb_font_t *font, void *font_data, hb_codepoint_t glyph,
               hb_glyph_extents_t *extents, void *user_data)
{
    FT_Face face = font_data;
    struct ass_shaper_metrics_data *metrics_priv = user_data;
    GlyphMetricsHashValue *metrics = get_cached_metrics(metrics_priv, face, glyph);

    if (!metrics)
        return 0;

    extents->x_bearing = metrics->metrics.horiBearingX;
    extents->y_bearing = metrics->metrics.horiBearingY;
    extents->width     = metrics->metrics.width;
    extents->height    = metrics->metrics.height;

    return 1;
}

static hb_bool_t
get_contour_point(hb_font_t *font, void *font_data, hb_codepoint_t glyph,
                     unsigned int point_index, hb_position_t *x,
                     hb_position_t *y, void *user_data)
{
    FT_Face face = font_data;
    int load_flags = FT_LOAD_DEFAULT | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH
        | FT_LOAD_IGNORE_TRANSFORM;

    if (FT_Load_Glyph(face, glyph, load_flags))
        return 0;

    if (point_index >= (unsigned)face->glyph->outline.n_points)
        return 0;

    *x = face->glyph->outline.points[point_index].x;
    *y = face->glyph->outline.points[point_index].y;

    return 1;
}

/**
 * \brief Retrieve HarfBuzz font from cache.
 * Create it from FreeType font, if needed.
 * \param info glyph cluster
 * \return HarfBuzz font
 */
static hb_font_t *get_hb_font(ASS_Shaper *shaper, GlyphInfo *info)
{
    ASS_Font *font = info->font;
    hb_font_t **hb_fonts;
    struct ass_shaper_metrics_data *metrics;

    if (!font->shaper_priv)
        font->shaper_priv = calloc(sizeof(ASS_ShaperFontData), 1);


    hb_fonts = font->shaper_priv->fonts;
    if (!hb_fonts[info->face_index]) {
        hb_font_funcs_t *funcs;

        hb_fonts[info->face_index] =
            hb_ft_font_create(font->faces[info->face_index], NULL);

        // set up cached metrics access
        font->shaper_priv->metrics_data[info->face_index] =
            calloc(sizeof(struct ass_shaper_metrics_data), 1);
        metrics = font->shaper_priv->metrics_data[info->face_index];
        metrics->metrics_cache = shaper->metrics_cache;
        metrics->vertical = info->font->desc.vertical;

        funcs = hb_font_funcs_create();
        font->shaper_priv->font_funcs[info->face_index] = funcs;
        hb_font_funcs_set_glyph_func(funcs, get_glyph,
                metrics, NULL);
        hb_font_funcs_set_glyph_h_advance_func(funcs, cached_h_advance,
                metrics, NULL);
        hb_font_funcs_set_glyph_v_advance_func(funcs, cached_v_advance,
                metrics, NULL);
        hb_font_funcs_set_glyph_h_origin_func(funcs, cached_h_origin,
                metrics, NULL);
        hb_font_funcs_set_glyph_v_origin_func(funcs, cached_v_origin,
                metrics, NULL);
        hb_font_funcs_set_glyph_h_kerning_func(funcs, get_h_kerning,
                metrics, NULL);
        hb_font_funcs_set_glyph_v_kerning_func(funcs, get_v_kerning,
                metrics, NULL);
        hb_font_funcs_set_glyph_extents_func(funcs, cached_extents,
                metrics, NULL);
        hb_font_funcs_set_glyph_contour_point_func(funcs, get_contour_point,
                metrics, NULL);
        hb_font_set_funcs(hb_fonts[info->face_index], funcs,
                font->faces[info->face_index], NULL);
    }

    // XXX: this is a rather crude hack
    const double ft_size = 256.0;
    ass_face_set_size(font->faces[info->face_index], ft_size);
    update_hb_size(hb_fonts[info->face_index], font->faces[info->face_index]);

    // update hash key for cached metrics
    metrics = font->shaper_priv->metrics_data[info->face_index];
    metrics->hash_key.font = info->font;
    metrics->hash_key.face_index = info->face_index;
    metrics->hash_key.size = info->font_size;
    metrics->hash_key.scale_x = double_to_d6(info->scale_x);
    metrics->hash_key.scale_y = double_to_d6(info->scale_y);

    return hb_fonts[info->face_index];
}

/**
 * \brief Map script to default language.
 *
 * This maps a script to a language, if a script has a representative
 * language it is typically used with. Otherwise, the invalid language
 * is returned.
 *
 * The mapping is similar to Pango's pango-language.c.
 *
 * \param script script tag
 * \return language tag
 */
static hb_language_t script_to_language(hb_script_t script)
{
    switch (script) {
        // Unicode 1.1
        case HB_SCRIPT_ARABIC: return hb_language_from_string("ar", -1); break;
        case HB_SCRIPT_ARMENIAN: return hb_language_from_string("hy", -1); break;
        case HB_SCRIPT_BENGALI: return hb_language_from_string("bn", -1); break;
        case HB_SCRIPT_CANADIAN_ABORIGINAL: return hb_language_from_string("iu", -1); break;
        case HB_SCRIPT_CHEROKEE: return hb_language_from_string("chr", -1); break;
        case HB_SCRIPT_COPTIC: return hb_language_from_string("cop", -1); break;
        case HB_SCRIPT_CYRILLIC: return hb_language_from_string("ru", -1); break;
        case HB_SCRIPT_DEVANAGARI: return hb_language_from_string("hi", -1); break;
        case HB_SCRIPT_GEORGIAN: return hb_language_from_string("ka", -1); break;
        case HB_SCRIPT_GREEK: return hb_language_from_string("el", -1); break;
        case HB_SCRIPT_GUJARATI: return hb_language_from_string("gu", -1); break;
        case HB_SCRIPT_GURMUKHI: return hb_language_from_string("pa", -1); break;
        case HB_SCRIPT_HANGUL: return hb_language_from_string("ko", -1); break;
        case HB_SCRIPT_HEBREW: return hb_language_from_string("he", -1); break;
        case HB_SCRIPT_HIRAGANA: return hb_language_from_string("ja", -1); break;
        case HB_SCRIPT_KANNADA: return hb_language_from_string("kn", -1); break;
        case HB_SCRIPT_KATAKANA: return hb_language_from_string("ja", -1); break;
        case HB_SCRIPT_LAO: return hb_language_from_string("lo", -1); break;
        case HB_SCRIPT_LATIN: return hb_language_from_string("en", -1); break;
        case HB_SCRIPT_MALAYALAM: return hb_language_from_string("ml", -1); break;
        case HB_SCRIPT_MONGOLIAN: return hb_language_from_string("mn", -1); break;
        case HB_SCRIPT_ORIYA: return hb_language_from_string("or", -1); break;
        case HB_SCRIPT_SYRIAC: return hb_language_from_string("syr", -1); break;
        case HB_SCRIPT_TAMIL: return hb_language_from_string("ta", -1); break;
        case HB_SCRIPT_TELUGU: return hb_language_from_string("te", -1); break;
        case HB_SCRIPT_THAI: return hb_language_from_string("th", -1); break;

        // Unicode 2.0
        case HB_SCRIPT_TIBETAN: return hb_language_from_string("bo", -1); break;

        // Unicode 3.0
        case HB_SCRIPT_ETHIOPIC: return hb_language_from_string("am", -1); break;
        case HB_SCRIPT_KHMER: return hb_language_from_string("km", -1); break;
        case HB_SCRIPT_MYANMAR: return hb_language_from_string("my", -1); break;
        case HB_SCRIPT_SINHALA: return hb_language_from_string("si", -1); break;
        case HB_SCRIPT_THAANA: return hb_language_from_string("dv", -1); break;

        // Unicode 3.2
        case HB_SCRIPT_BUHID: return hb_language_from_string("bku", -1); break;
        case HB_SCRIPT_HANUNOO: return hb_language_from_string("hnn", -1); break;
        case HB_SCRIPT_TAGALOG: return hb_language_from_string("tl", -1); break;
        case HB_SCRIPT_TAGBANWA: return hb_language_from_string("tbw", -1); break;

        // Unicode 4.0
        case HB_SCRIPT_UGARITIC: return hb_language_from_string("uga", -1); break;

        // Unicode 4.1
        case HB_SCRIPT_BUGINESE: return hb_language_from_string("bug", -1); break;
        case HB_SCRIPT_OLD_PERSIAN: return hb_language_from_string("peo", -1); break;
        case HB_SCRIPT_SYLOTI_NAGRI: return hb_language_from_string("syl", -1); break;

        // Unicode 5.0
        case HB_SCRIPT_NKO: return hb_language_from_string("nko", -1); break;

        // no representative language exists
        default: return HB_LANGUAGE_INVALID; break;
    }
}

/**
 * \brief Determine language to be used for shaping a run.
 *
 * \param shaper shaper instance
 * \param script script tag associated with run
 * \return language tag
 */
static hb_language_t
hb_shaper_get_run_language(ASS_Shaper *shaper, hb_script_t script)
{
    hb_language_t lang;

    // override set, use it
    if (shaper->language != HB_LANGUAGE_INVALID)
        return shaper->language;

    // get default language for given script
    lang = script_to_language(script);

    // no dice, use system default
    if (lang == HB_LANGUAGE_INVALID)
        lang = hb_language_get_default();

    return lang;
}

/**
 * \brief Shape event text with HarfBuzz. Full OpenType shaping.
 * \param glyphs glyph clusters
 * \param len number of clusters
 */
static void shape_harfbuzz(ASS_Shaper *shaper, GlyphInfo *glyphs, size_t len)
{
    int i, j;
    int run = 0;
    struct {
        int offset;
        int end;
        hb_buffer_t *buf;
        hb_font_t *font;
    } runs[MAX_RUNS];
    const double ft_size = 256.0;

    for (i = 0; i < len && run < MAX_RUNS; i++, run++) {
        // get length and level of the current run
        int k = i;
        int level = glyphs[i].shape_run_id;
        int direction = shaper->emblevels[k] % 2;
        hb_script_t script = glyphs[i].script;
        while (i < (len - 1) && level == glyphs[i+1].shape_run_id)
            i++;
        runs[run].offset = k;
        runs[run].end    = i;
        runs[run].buf    = hb_buffer_create();
        runs[run].font   = get_hb_font(shaper, glyphs + k);
        set_run_features(shaper, glyphs + k);
        hb_buffer_pre_allocate(runs[run].buf, i - k + 1);
        hb_buffer_set_direction(runs[run].buf, direction ? HB_DIRECTION_RTL :
                HB_DIRECTION_LTR);
        hb_buffer_set_language(runs[run].buf,
                hb_shaper_get_run_language(shaper, script));
        hb_buffer_set_script(runs[run].buf, script);
        hb_buffer_add_utf32(runs[run].buf, shaper->event_text + k, i - k + 1,
                0, i - k + 1);
        hb_shape(runs[run].font, runs[run].buf, shaper->features,
                shaper->n_features);
    }

    // Initialize: skip all glyphs, this is undone later as needed
    for (i = 0; i < len; i++)
        glyphs[i].skip = 1;

    // Update glyph indexes, positions and advances from the shaped runs
    for (i = 0; i < run; i++) {
        int num_glyphs = hb_buffer_get_length(runs[i].buf);
        hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(runs[i].buf, NULL);
        hb_glyph_position_t *pos    = hb_buffer_get_glyph_positions(runs[i].buf, NULL);

        for (j = 0; j < num_glyphs; j++) {
            int idx = glyph_info[j].cluster + runs[i].offset;
            GlyphInfo *info = glyphs + idx;
            GlyphInfo *root = info;

            // if we have more than one glyph per cluster, allocate a new one
            // and attach to the root glyph
            if (info->skip == 0) {
                while (info->next)
                    info = info->next;
                info->next = malloc(sizeof(GlyphInfo));
                memcpy(info->next, info, sizeof(GlyphInfo));
                info = info->next;
                info->next = NULL;
            }

            // set position and advance
            info->skip = 0;
            info->glyph_index = glyph_info[j].codepoint;
            info->offset.x    = pos[j].x_offset * info->scale_x * (info->font_size / ft_size);
            info->offset.y    = -pos[j].y_offset * info->scale_y * (info->font_size / ft_size);
            info->advance.x   = pos[j].x_advance * info->scale_x * (info->font_size / ft_size);
            info->advance.y   = -pos[j].y_advance * info->scale_y * (info->font_size / ft_size);

            // accumulate advance in the root glyph
            root->cluster_advance.x += info->advance.x;
            root->cluster_advance.y += info->advance.y;
        }
    }

    // Free runs and associated data
    for (i = 0; i < run; i++) {
        hb_buffer_destroy(runs[i].buf);
    }

}

/**
 * \brief Determine script property of all characters. Characters of script
 * common and inherited get their script from their context.
 *
 */
void ass_shaper_determine_script(ASS_Shaper *shaper, GlyphInfo *glyphs,
                                  size_t len)
{
    int i;
    int backwards_scan = 0;
    hb_unicode_funcs_t *ufuncs = hb_unicode_funcs_get_default();
    hb_script_t last_script = HB_SCRIPT_UNKNOWN;

    // determine script (forward scan)
    for (i = 0; i < len; i++) {
        GlyphInfo *info = glyphs + i;
        info->script = hb_unicode_script(ufuncs, info->symbol);

        // common/inherit codepoints inherit script from context
        if (info->script == HB_SCRIPT_COMMON ||
                info->script == HB_SCRIPT_INHERITED) {
            // unknown is not a valid context
            if (last_script != HB_SCRIPT_UNKNOWN)
                info->script = last_script;
            else
                // do a backwards scan to check if next codepoint
                // contains a valid script for context
                backwards_scan = 1;
        } else {
            last_script = info->script;
        }
    }

    // determine script (backwards scan, if needed)
    last_script = HB_SCRIPT_UNKNOWN;
    for (i = len - 1; i >= 0 && backwards_scan; i--) {
        GlyphInfo *info = glyphs + i;

        // common/inherit codepoints inherit script from context
        if (info->script == HB_SCRIPT_COMMON ||
                info->script == HB_SCRIPT_INHERITED) {
            // unknown script is not a valid context
            if (last_script != HB_SCRIPT_UNKNOWN)
                info->script = last_script;
        } else {
            last_script = info->script;
        }
    }
}
#endif

/**
 * \brief Shape event text with FriBidi. Does mirroring and simple
 * Arabic shaping.
 * \param len number of clusters
 */
static void shape_fribidi(ASS_Shaper *shaper, GlyphInfo *glyphs, size_t len)
{
    int i;
    FriBidiJoiningType *joins = calloc(sizeof(*joins), len);

    // shape on codepoint level
    fribidi_get_joining_types(shaper->event_text, len, joins);
    fribidi_join_arabic(shaper->ctypes, len, shaper->emblevels, joins);
    fribidi_shape(FRIBIDI_FLAGS_DEFAULT | FRIBIDI_FLAGS_ARABIC,
            shaper->emblevels, len, joins, shaper->event_text);

    // update indexes
    for (i = 0; i < len; i++) {
        GlyphInfo *info = glyphs + i;
        FT_Face face = info->font->faces[info->face_index];
        info->symbol = shaper->event_text[i];
        info->glyph_index = FT_Get_Char_Index(face, shaper->event_text[i]);
    }

    free(joins);
}

/**
 * \brief Toggle kerning for HarfBuzz shaping.
 * NOTE: currently only works with OpenType fonts, the TrueType fallback *always*
 * kerns. It's a bug in HarfBuzz.
 */
void ass_shaper_set_kerning(ASS_Shaper *shaper, int kern)
{
#ifdef CONFIG_HARFBUZZ
    shaper->features[KERN].value = !!kern;
#endif
}

/**
 * \brief Find shape runs according to the event's selected fonts
 */
void ass_shaper_find_runs(ASS_Shaper *shaper, ASS_Renderer *render_priv,
                          GlyphInfo *glyphs, size_t len)
{
    int i;
    int shape_run = 0;

#ifdef CONFIG_HARFBUZZ
    ass_shaper_determine_script(shaper, glyphs, len);
#endif

    // find appropriate fonts for the shape runs
    for (i = 0; i < len; i++) {
        GlyphInfo *last = glyphs + i - 1;
        GlyphInfo *info = glyphs + i;
        // skip drawings
        if (info->symbol == 0xfffc)
            continue;
        // set size and get glyph index
        ass_font_get_index(render_priv->fontconfig_priv, info->font,
                info->symbol, &info->face_index, &info->glyph_index);
        // shape runs share the same font face and size
        if (i > 0 && (last->font != info->font ||
                    last->font_size != info->font_size ||
                    last->face_index != info->face_index ||
                    last->script != info->script))
            shape_run++;
        info->shape_run_id = shape_run;
    }
}

/**
 * \brief Set base direction (paragraph direction) of the text.
 * \param dir base direction
 */
void ass_shaper_set_base_direction(ASS_Shaper *shaper, FriBidiParType dir)
{
    shaper->base_direction = dir;
}

/**
 * \brief Set language hint. Some languages have specific character variants,
 * like Serbian Cyrillic.
 * \param lang ISO 639-1 two-letter language code
 */
void ass_shaper_set_language(ASS_Shaper *shaper, const char *code)
{
#ifdef CONFIG_HARFBUZZ
    hb_language_t lang;

    if (code)
        lang = hb_language_from_string(code, -1);
    else
        lang = HB_LANGUAGE_INVALID;

    shaper->language = lang;
#endif
}

/**
 * Set shaping level. Essentially switches between FriBidi and HarfBuzz.
 */
void ass_shaper_set_level(ASS_Shaper *shaper, ASS_ShapingLevel level)
{
    shaper->shaping_level = level;
}

/**
  * \brief Remove all zero-width invisible characters from the text.
  * \param text_info text
  */
static void ass_shaper_skip_characters(TextInfo *text_info)
{
    int i;
    GlyphInfo *glyphs = text_info->glyphs;

    for (i = 0; i < text_info->length; i++) {
        // Skip direction override control characters
        if ((glyphs[i].symbol <= 0x202e && glyphs[i].symbol >= 0x202a)
                || (glyphs[i].symbol <= 0x200f && glyphs[i].symbol >= 0x200b)
                || (glyphs[i].symbol <= 0x2063 && glyphs[i].symbol >= 0x2060)
                || glyphs[i].symbol == 0xfeff
                || glyphs[i].symbol == 0x00ad
                || glyphs[i].symbol == 0x034f) {
            glyphs[i].symbol = 0;
            glyphs[i].skip++;
        }
    }
}

/**
 * \brief Shape an event's text. Calculates directional runs and shapes them.
 * \param text_info event's text
 */
void ass_shaper_shape(ASS_Shaper *shaper, TextInfo *text_info)
{
    int i, last_break;
    FriBidiParType dir;
    GlyphInfo *glyphs = text_info->glyphs;

    check_allocations(shaper, text_info->length);

    // Get bidi character types and embedding levels
    last_break = 0;
    for (i = 0; i < text_info->length; i++) {
        shaper->event_text[i] = glyphs[i].symbol;
        // embedding levels should be calculated paragraph by paragraph
        if (glyphs[i].symbol == '\n' || i == text_info->length - 1) {
            dir = shaper->base_direction;
            fribidi_get_bidi_types(shaper->event_text + last_break,
                    i - last_break + 1, shaper->ctypes + last_break);
            fribidi_get_par_embedding_levels(shaper->ctypes + last_break,
                    i - last_break + 1, &dir, shaper->emblevels + last_break);
            last_break = i + 1;
        }
    }

    // add embedding levels to shape runs for final runs
    for (i = 0; i < text_info->length; i++) {
        glyphs[i].shape_run_id += shaper->emblevels[i];
    }

#ifdef CONFIG_HARFBUZZ
    switch (shaper->shaping_level) {
    case ASS_SHAPING_SIMPLE:
        shape_fribidi(shaper, glyphs, text_info->length);
        ass_shaper_skip_characters(text_info);
        break;
    case ASS_SHAPING_COMPLEX:
        shape_harfbuzz(shaper, glyphs, text_info->length);
        break;
    }
#else
        shape_fribidi(shaper, glyphs, text_info->length);
        ass_shaper_skip_characters(text_info);
#endif
}

/**
 * \brief Create a new shaper instance and preallocate data structures
 * \param prealloc preallocation size
 */
ASS_Shaper *ass_shaper_new(size_t prealloc)
{
    ASS_Shaper *shaper = calloc(sizeof(*shaper), 1);

    shaper->base_direction = FRIBIDI_PAR_ON;
    check_allocations(shaper, prealloc);

#ifdef CONFIG_HARFBUZZ
    init_features(shaper);
    shaper->metrics_cache = ass_glyph_metrics_cache_create();
#endif

    return shaper;
}


/**
 * \brief clean up additional data temporarily needed for shaping and
 * (e.g. additional glyphs allocated)
 */
void ass_shaper_cleanup(ASS_Shaper *shaper, TextInfo *text_info)
{
    int i;

    for (i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        info = info->next;
        while (info) {
            GlyphInfo *next = info->next;
            free(info);
            info = next;
        }
    }
}

/**
 * \brief Calculate reorder map to render glyphs in visual order
 */
FriBidiStrIndex *ass_shaper_reorder(ASS_Shaper *shaper, TextInfo *text_info)
{
    int i;

    // Initialize reorder map
    for (i = 0; i < text_info->length; i++)
        shaper->cmap[i] = i;

    // Create reorder map line-by-line
    for (i = 0; i < text_info->n_lines; i++) {
        LineInfo *line = text_info->lines + i;
        FriBidiParType dir = FRIBIDI_PAR_ON;

        fribidi_reorder_line(0,
                shaper->ctypes + line->offset, line->len, 0, dir,
                shaper->emblevels + line->offset, NULL,
                shaper->cmap + line->offset);
    }

    return shaper->cmap;
}

/**
 * \brief Resolve a Windows font charset number to a suitable
 * base direction. 177 and 178 are Hebrew and Arabic respectively, and
 * they map to RTL. Everything else maps to LTR for compatibility
 * reasons. The special value -1, which is not a legal Windows font charset
 * number, can be used for autodetection.
 * \param enc Windows font encoding
 */
FriBidiParType resolve_base_direction(int enc)
{
    switch (enc) {
        case -1:
            return FRIBIDI_PAR_ON;
        case 177:
        case 178:
            return FRIBIDI_PAR_RTL;
        default:
            return FRIBIDI_PAR_LTR;
    }
}
