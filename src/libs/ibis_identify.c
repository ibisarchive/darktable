/*
    This file is part of Ibis Archive, a darktable distribution.
    Copyright (C) 2026 Ibis Archive contributors.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

// identify birds: lighttable module
//
// runs an on-device species classifier over the images to act on and
// tags each one Birds|Species|<name>. Those tags are what the eBird
// checklist export (data/lua/ibis/ebird.lua) reads, and they travel in
// the XMP sidecar like any other keyword.
//
// the classifier is an ONNX model in the user's AI models folder
// (<user data>/darktable/models/<model_id>/): config.json, model.onnx
// and labels.txt, one label per output column. darktable's own AI
// backend (src/ai) loads and runs it, so CPU/DirectML/CUDA follow the
// user's provider preference like every other model.
//
// two kinds of classifier are understood, told apart by "arch" in the
// model's config.json:
//   - a fixed head (Kestrel species head v1, arch kestrel-species-v1):
//     'data' float32 NCHW 1x3x300x300, RGB, 0..255, not normalised;
//     'model_output' 1xN softmax probabilities, row i = labels.txt line i
//   - an embedding model (BioCLIP 2, arch bioclip): CLIP-normalised
//     1x3x224x224 in, a 1xD embedding out. text_embeds.bin beside the
//     model holds one L2-normalised D-vector per labels.txt line (every
//     eBird species); cosine similarity times the model's logit scale,
//     softmaxed, is the probability. the label set is data, so the same
//     code serves any taxonomy. tools/ibis/export_bioclip.py builds it
//
// a bird is small in most frames, and the classifier was trained on
// crops, so a detector runs first when its model is present (model id
// detect-animals, MegaDetector v1000 cedar: 'images' float32
// 1x3x640x640 RGB 0..1 letterboxed; 'predictions' 1x7x8400 = cx,cy,w,h
// in input pixels then class scores animal, person, vehicle). the best
// animal box, squared and padded, is what the classifier sees. without
// a detector, or with nothing found, the frame's center square is used.
//
// the work runs as a dt_control_job on the user background queue; the
// GUI only reads the result through g_idle_add.

#include "common/act_on.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/signal.h"
#include "ai/backend.h"
#include "bauhaus/bauhaus.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include "common/curl_tools.h"
#include "common/image_cache.h"

#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

DT_MODULE(1)

#define CONF_MIN_SCORE   "plugins/lighttable/ibis_identify/min_score"
#define CONF_MODEL_ID    "plugins/lighttable/ibis_identify/model_id"
#define CONF_DETECTOR_ID "plugins/lighttable/ibis_identify/detector_id"
#define CONF_REGION_LIST "plugins/lighttable/ibis_identify/region_list"
#define CONF_API_KEY     "plugins/lighttable/ibis_identify/ebird_api_key"
#define CONF_COUNTRY     "plugins/lighttable/ibis_identify/country"
#define CONF_AUTO_REGION "plugins/lighttable/ibis_identify/auto_region"

// the classifier's runners-up, kept as internal tags (darktable| tags are
// neither exported nor shown in the tag tree): darktable|ibis|candidate|<rank>|<name>|<percent>
#define CAND_ROOT     "darktable|ibis|candidate|"
#define N_CANDIDATES  3

#define EBIRD_API     "https://api.ebird.org/v2"
#define REGION_CACHE_DAYS 30

#define TAG_ROOT      "Birds|Species|"
#define TAG_UNKNOWN   "Birds|Species|Unidentified"
#define INPUT_SIDE    300

#define DET_SIDE      640
#define CLIP_SIDE     224
// OpenCLIP normalisation, as in open_clip_config.json of imageomics/bioclip-2
static const float CLIP_MEAN[3] = { 0.48145466f, 0.4578275f, 0.40821073f };
static const float CLIP_STD[3]  = { 0.26862954f, 0.26130258f, 0.27577711f };
#define DET_MIN_CONF  0.25f   // MegaDetector's usual threshold
#define DET_PAD       0.15f   // margin around the box before squaring
#define DET_CLASS_ANIMAL 0

// a detection in thumbnail pixel coordinates
typedef struct dt_ibis_box_t
{
  float x0, y0, x1, y1;
  float conf;
} dt_ibis_box_t;

// a square region of the thumbnail to classify
typedef struct dt_ibis_rect_t
{
  int x, y, side;
} dt_ibis_rect_t;

static gint dt_ibis_compare_double(const double *a, const double *b)
{
  return (*a > *b) - (*a < *b);
}

typedef struct dt_lib_ibis_identify_t
{
  GtkWidget *run_button;
  GtkWidget *min_score;
  GtkWidget *status;
  GtkWidget *region_label;
  GtkWidget *api_key;
  GtkWidget *country;
  dt_gui_collapsible_section_t settings;
  // review: the hovered (else acted-on) frame's species and runners-up
  GtkWidget *review_title;
  GtkWidget *candidate[N_CANDIDATES];
  GtkWidget *candidate_name[N_CANDIDATES];  // species, left
  GtkWidget *candidate_pct[N_CANDIDATES];   // percentage, right
  GtkWidget *no_bird;
  dt_imgid_t review_imgid;
} dt_lib_ibis_identify_t;

typedef struct dt_ibis_job_t
{
  GList *images;          // imgids to classify
  float min_score;
  char *model_id;
  char *detector_id;
  char *api_key;          // eBird API key, for the region lookup
  char *country;          // fallback region when no frame has a position
  gboolean auto_region;
  dt_lib_module_t *self;
  // results, filled by the job, read by the idle callback
  int tagged;
  int unsure;
  int failed;
  char *region_note;      // "Rogaland (NO-11), 422 species" or why there was none
  char *error;            // NULL when the model loaded and ran
} dt_ibis_job_t;

const char *name(dt_lib_module_t *self)
{
  return _("identify birds");
}

const char *description(dt_lib_module_t *self)
{
  return _("classify the bird in each selected image\n"
           "and tag it Birds|Species|<name>");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 900; // the top of the lighttable's right panel: after culling, this is the next step
}

// --- labels ---------------------------------------------------------------

// a file beside the model, in whichever models folder the backend uses
static char *_model_file(const char *model_id, const char *name)
{
  char *dir = dt_ai_resolve_models_path_override();
  char *path = dir
    ? g_build_filename(dir, model_id, name, NULL)
    : g_build_filename(g_get_user_data_dir(), "darktable", "models", model_id, name, NULL);
  g_free(dir);
  return path;
}

// --- eBird region lookup ---------------------------------------------------
//
// what Merlin does with your location: the classifier only considers
// species that occur where the frames were taken. the frames' median
// position -> nearest eBird hotspot -> its region code (NO-11) -> the
// region's species list, cached beside the models for a month

static size_t _curl_collect(void *ptr, size_t size, size_t nmemb, void *userdata)
{
  g_string_append_len((GString *)userdata, ptr, size * nmemb);
  return size * nmemb;
}

// GET with the eBird key header; NULL on any failure
static char *_ebird_get(const char *url, const char *api_key)
{
  CURL *curl = curl_easy_init();
  if(!curl) return NULL;
  dt_curl_init(curl, FALSE);
  GString *body = g_string_new(NULL);
  char *hdr = g_strdup_printf("x-ebirdapitoken: %s", api_key);
  struct curl_slist *headers = curl_slist_append(NULL, hdr);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  const CURLcode res = curl_easy_perform(curl);
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  curl_slist_free_all(headers);
  g_free(hdr);
  curl_easy_cleanup(curl);
  if(res != CURLE_OK || http != 200)
  {
    dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] eBird %s: curl=%d http=%ld", url, res, http);
    g_string_free(body, TRUE);
    return NULL;
  }
  return g_string_free(body, FALSE);
}

static char *_regions_dir(void)
{
  char *dir = dt_ai_resolve_models_path_override();
  char *path = dir
    ? g_build_filename(dir, "regions", NULL)
    : g_build_filename(g_get_user_data_dir(), "darktable", "models", "regions", NULL);
  g_free(dir);
  g_mkdir_with_parents(path, 0755);
  return path;
}

static gboolean _fresh(const char *path)
{
  GStatBuf st;
  if(g_stat(path, &st) != 0) return FALSE;
  return (g_get_real_time() / G_USEC_PER_SEC - st.st_mtime) < (gint64)REGION_CACHE_DAYS * 86400;
}

// the median position of the frames that have one; FALSE when none does
static gboolean _median_position(GList *images, double *lat, double *lon)
{
  GArray *lats = g_array_new(FALSE, FALSE, sizeof(double));
  GArray *lons = g_array_new(FALSE, FALSE, sizeof(double));
  for(GList *l = images; l; l = g_list_next(l))
  {
    dt_image_geoloc_t geo;
    dt_image_get_location(GPOINTER_TO_INT(l->data), &geo);
    if(isfinite(geo.latitude) && isfinite(geo.longitude)
       && !(geo.latitude == 0.0 && geo.longitude == 0.0))
    {
      g_array_append_val(lats, geo.latitude);
      g_array_append_val(lons, geo.longitude);
    }
  }
  const gboolean ok = lats->len > 0;
  if(ok)
  {
    g_array_sort(lats, (GCompareFunc)dt_ibis_compare_double);
    g_array_sort(lons, (GCompareFunc)dt_ibis_compare_double);
    *lat = g_array_index(lats, double, lats->len / 2);
    *lon = g_array_index(lons, double, lons->len / 2);
  }
  g_array_free(lats, TRUE);
  g_array_free(lons, TRUE);
  return ok;
}

// the region of the nearest hotspot to a position: "NO-11". cached per
// 0.01 degree cell so a revisited place costs no request. *place gets
// the hotspot's name
static char *_region_for_position(const double lat, const double lon, const char *api_key, char **place)
{
  char *dir = _regions_dir();
  char *cache = g_strdup_printf("%s/geo_%.2f_%.2f.txt", dir, lat, lon);
  g_free(dir);
  gchar *cached = NULL;
  if(_fresh(cache) && g_file_get_contents(cache, &cached, NULL, NULL))
  {
    char **parts = g_strsplit(g_strstrip(cached), "\t", 2);
    char *region = g_strdup(parts[0]);
    if(place && parts[1]) *place = g_strdup(parts[1]);
    g_strfreev(parts);
    g_free(cached);
    g_free(cache);
    return region[0] ? region : (g_free(region), NULL);
  }
  g_free(cached);

  char *url = g_strdup_printf("%s/ref/hotspot/geo?lat=%.4f&lng=%.4f&dist=50&fmt=json", EBIRD_API, lat, lon);
  char *body = _ebird_get(url, api_key);
  g_free(url);
  if(!body) { g_free(cache); return NULL; }

  char *region = NULL;
  JsonParser *parser = json_parser_new();
  if(json_parser_load_from_data(parser, body, -1, NULL))
  {
    JsonNode *root = json_parser_get_root(parser);
    if(root && JSON_NODE_HOLDS_ARRAY(root))
    {
      JsonArray *arr = json_node_get_array(root);
      double best = 1e12;
      const guint n = json_array_get_length(arr);
      for(guint i = 0; i < n; i++)
      {
        JsonObject *h = json_array_get_object_element(arr, i);
        if(!h || !json_object_has_member(h, "lat") || !json_object_has_member(h, "lng")) continue;
        const double hl = json_object_get_double_member(h, "lat");
        const double hg = json_object_get_double_member(h, "lng");
        const double dx = (hg - lon) * 111.0 * cos(lat * M_PI / 180.0);
        const double dy = (hl - lat) * 111.0;
        const double d = dx * dx + dy * dy;
        if(d < best)
        {
          best = d;
          g_free(region);
          const char *code = json_object_has_member(h, "subnational1Code")
            ? json_object_get_string_member(h, "subnational1Code")
            : (json_object_has_member(h, "countryCode") ? json_object_get_string_member(h, "countryCode") : NULL);
          region = g_strdup(code ? code : "");
          if(place)
          {
            g_free(*place);
            *place = g_strdup(json_object_has_member(h, "locName") ? json_object_get_string_member(h, "locName") : "");
          }
        }
      }
    }
  }
  g_object_unref(parser);
  g_free(body);
  if(region && region[0])
  {
    char *line = g_strdup_printf("%s\t%s\n", region, place && *place ? *place : "");
    g_file_set_contents(cache, line, -1, NULL);
    g_free(line);
  }
  else
  {
    g_free(region);
    region = NULL;
  }
  g_free(cache);
  return region;
}

// the species codes eBird lists for a region, one per line, cached
static GHashTable *_species_codes_for_region(const char *region, const char *api_key)
{
  char *dir = _regions_dir();
  char *cache = g_strdup_printf("%s/%s.codes", dir, region);
  g_free(dir);
  gchar *text = NULL;
  if(!(_fresh(cache) && g_file_get_contents(cache, &text, NULL, NULL)))
  {
    g_free(text);
    text = NULL;
    char *url = g_strdup_printf("%s/product/spplist/%s", EBIRD_API, region);
    char *body = _ebird_get(url, api_key);
    g_free(url);
    if(body)
    {
      GString *lines = g_string_new(NULL);
      JsonParser *parser = json_parser_new();
      if(json_parser_load_from_data(parser, body, -1, NULL))
      {
        JsonNode *root = json_parser_get_root(parser);
        if(root && JSON_NODE_HOLDS_ARRAY(root))
        {
          JsonArray *arr = json_node_get_array(root);
          const guint n = json_array_get_length(arr);
          for(guint i = 0; i < n; i++)
            g_string_append_printf(lines, "%s\n", json_array_get_string_element(arr, i));
        }
      }
      g_object_unref(parser);
      g_free(body);
      if(lines->len) g_file_set_contents(cache, lines->str, -1, NULL);
      text = g_string_free(lines, FALSE);
    }
  }
  g_free(cache);
  if(!text || !text[0]) { g_free(text); return NULL; }

  GHashTable *codes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  char **lines = g_strsplit(text, "\n", -1);
  for(int i = 0; lines[i]; i++)
  {
    g_strstrip(lines[i]);
    if(lines[i][0]) g_hash_table_add(codes, g_strdup(lines[i]));
  }
  g_strfreev(lines);
  g_free(text);
  return codes;
}

// species.csv beside the model (species_code,scientific_name,common_name,...)
// turns eBird's codes into the model's label names. the global model has
// one; for another model the global model's copy is the next best answer
static guint8 *_mask_from_codes(GHashTable *codes, const char *model_id, char **labels, const int n_labels, int *hits)
{
  *hits = 0;
  char *path = _model_file(model_id, "species.csv");
  gchar *text = NULL;
  if(!g_file_get_contents(path, &text, NULL, NULL))
  {
    g_free(path);
    path = _model_file("classify-birds-global", "species.csv");
    if(!g_file_get_contents(path, &text, NULL, NULL))
    {
      g_free(path);
      return NULL;
    }
  }
  g_free(path);
  GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  char **lines = g_strsplit(text, "\n", -1);
  for(int i = 1; lines[i]; i++)
  {
    char **f = g_strsplit(g_strstrip(lines[i]), ",", 4);
    if(f[0] && f[1] && f[2] && g_hash_table_contains(codes, f[0]))
      g_hash_table_add(names, g_strdup(f[2]));
    g_strfreev(f);
  }
  g_strfreev(lines);
  g_free(text);
  guint8 *mask = g_malloc0(n_labels);
  for(int i = 0; i < n_labels; i++)
    if(g_hash_table_contains(names, labels[i])) { mask[i] = 1; (*hits)++; }
  g_hash_table_destroy(names);
  if(*hits == 0) { g_free(mask); return NULL; }
  return mask;
}

// the region list (conf plugins/lighttable/ibis_identify/region_list): a
// text file with one species common name per line, as eBird spells it.
// returns a mask over the labels, or NULL when there is no list
static guint8 *_load_region_mask(char **labels, const int n_labels)
{
  char *path = dt_conf_get_string(CONF_REGION_LIST);
  if(!path || !path[0])
  {
    g_free(path);
    return NULL;
  }
  gchar *text = NULL;
  if(!g_file_get_contents(path, &text, NULL, NULL))
  {
    dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] region list %s not readable; using every species", path);
    g_free(path);
    return NULL;
  }
  GHashTable *names = g_hash_table_new(g_str_hash, g_str_equal);
  char **lines = g_strsplit(text, "\n", -1);
  for(int i = 0; lines[i]; i++)
  {
    g_strstrip(lines[i]);
    if(lines[i][0] && lines[i][0] != '#') g_hash_table_add(names, lines[i]);
  }
  guint8 *mask = g_malloc0(n_labels);
  int hits = 0;
  for(int i = 0; i < n_labels; i++)
    if(g_hash_table_contains(names, labels[i])) { mask[i] = 1; hits++; }
  dt_print(DT_DEBUG_AI, "[ibis_identify] region list %s: %d of %d species", path, hits, n_labels);
  g_hash_table_destroy(names);
  g_strfreev(lines);
  g_free(text);
  g_free(path);
  if(hits == 0)
  {
    // a list that matches nothing would silence the classifier
    g_free(mask);
    return NULL;
  }
  return mask;
}

// labels.txt: one common name per line, UTF-8, optional BOM; the line
// index is the output column
static char **_load_labels(const char *model_id, int *count)
{
  *count = 0;
  char *path = _model_file(model_id, "labels.txt");

  gchar *text = NULL;
  if(!g_file_get_contents(path, &text, NULL, NULL))
  {
    dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] no labels at %s", path);
    g_free(path);
    return NULL;
  }
  g_free(path);

  const char *start = text;
  if(g_str_has_prefix(start, "\xEF\xBB\xBF")) start += 3;
  char **lines = g_strsplit(start, "\n", -1);
  g_free(text);

  int n = 0;
  for(; lines[n]; n++)
    g_strstrip(lines[n]);
  // a trailing empty line is the file's newline, not a label
  while(n > 0 && lines[n - 1][0] == '\0') n--;
  *count = n;
  return lines;
}

// --- one frame ------------------------------------------------------------

// bilinear sample of one RGB pixel, clamped to the image
static inline void _sample(const uint8_t *rgba, const int w, const int h,
                           const float sx, const float sy, float *rgb)
{
  const int x0 = CLAMP((int)floorf(sx), 0, w - 1);
  const int y0 = CLAMP((int)floorf(sy), 0, h - 1);
  const int x1 = MIN(x0 + 1, w - 1);
  const int y1 = MIN(y0 + 1, h - 1);
  const float fx = CLAMP(sx - x0, 0.0f, 1.0f);
  const float fy = CLAMP(sy - y0, 0.0f, 1.0f);
  const uint8_t *p00 = rgba + 4 * (y0 * w + x0);
  const uint8_t *p01 = rgba + 4 * (y0 * w + x1);
  const uint8_t *p10 = rgba + 4 * (y1 * w + x0);
  const uint8_t *p11 = rgba + 4 * (y1 * w + x1);
  for(int c = 0; c < 3; c++)
  {
    const float top = p00[c] * (1.0f - fx) + p01[c] * fx;
    const float bot = p10[c] * (1.0f - fx) + p11[c] * fx;
    rgb[c] = top * (1.0f - fy) + bot * fy;
  }
}

// letterbox the thumbnail into the detector's square input (RGB 0..1,
// gray padding as in YOLO training) and report the mapping back
static void _prepare_detector_input(const uint8_t *rgba, const int w, const int h,
                                    float *out, float *scale, int *pad_x, int *pad_y)
{
  *scale = MIN((float)DET_SIDE / (float)w, (float)DET_SIDE / (float)h);
  const int nw = MAX(1, (int)roundf(w * *scale));
  const int nh = MAX(1, (int)roundf(h * *scale));
  *pad_x = (DET_SIDE - nw) / 2;
  *pad_y = (DET_SIDE - nh) / 2;
  const int plane = DET_SIDE * DET_SIDE;
  const float pad = 114.0f / 255.0f;

  for(int y = 0; y < DET_SIDE; y++)
  {
    for(int x = 0; x < DET_SIDE; x++)
    {
      const int o = y * DET_SIDE + x;
      const int ix = x - *pad_x;
      const int iy = y - *pad_y;
      if(ix < 0 || iy < 0 || ix >= nw || iy >= nh)
      {
        out[o] = out[plane + o] = out[2 * plane + o] = pad;
        continue;
      }
      float rgb[3];
      _sample(rgba, w, h, (ix + 0.5f) / *scale - 0.5f, (iy + 0.5f) / *scale - 0.5f, rgb);
      out[o] = rgb[0] / 255.0f;
      out[plane + o] = rgb[1] / 255.0f;
      out[2 * plane + o] = rgb[2] / 255.0f;
    }
  }
}

// the best animal box, in thumbnail pixels; FALSE when nothing scores
static gboolean _detect(dt_ai_context_t *det, const uint8_t *rgba, const int w, const int h,
                        float *det_in, float *det_out, const int n_ch, const int n_anchors,
                        dt_ibis_box_t *box)
{
  float scale;
  int pad_x, pad_y;
  _prepare_detector_input(rgba, w, h, det_in, &scale, &pad_x, &pad_y);

  int64_t in_shape[4] = { 1, 3, DET_SIDE, DET_SIDE };
  int64_t out_shape[3] = { 1, n_ch, n_anchors };
  dt_ai_tensor_t in = { .data = det_in, .type = DT_AI_FLOAT, .shape = in_shape, .ndim = 4 };
  dt_ai_tensor_t out = { .data = det_out, .type = DT_AI_FLOAT, .shape = out_shape, .ndim = 3 };
  if(dt_ai_run(det, &in, 1, &out, 1) != 0)
    return FALSE;

  // layout is channels-first: value(ch, i) = det_out[ch * n_anchors + i]
  int best = -1;
  float best_conf = DET_MIN_CONF;
  float max_coord = 0.0f;
  for(int i = 0; i < n_anchors; i++)
  {
    const float conf = det_out[(4 + DET_CLASS_ANIMAL) * n_anchors + i];
    if(conf >= best_conf)
    {
      best_conf = conf;
      best = i;
    }
    for(int k = 0; k < 4; k++)
      max_coord = MAX(max_coord, det_out[k * n_anchors + i]);
  }
  if(best < 0) return FALSE;

  // boxes are in input pixels, or normalized if the export chose so
  const float unit = (max_coord <= 2.0f) ? (float)DET_SIDE : 1.0f;
  const float cx = det_out[0 * n_anchors + best] * unit;
  const float cy = det_out[1 * n_anchors + best] * unit;
  const float bw = det_out[2 * n_anchors + best] * unit;
  const float bh = det_out[3 * n_anchors + best] * unit;
  box->x0 = (cx - bw / 2.0f - pad_x) / scale;
  box->x1 = (cx + bw / 2.0f - pad_x) / scale;
  box->y0 = (cy - bh / 2.0f - pad_y) / scale;
  box->y1 = (cy + bh / 2.0f - pad_y) / scale;
  box->conf = best_conf;
  return TRUE;
}

// the square the classifier sees: the padded detection, or the center
static dt_ibis_rect_t _crop_rect(const int w, const int h, const dt_ibis_box_t *box)
{
  dt_ibis_rect_t r;
  if(!box)
  {
    r.side = MIN(w, h);
    r.x = (w - r.side) / 2;
    r.y = (h - r.side) / 2;
    return r;
  }
  const float bw = box->x1 - box->x0;
  const float bh = box->y1 - box->y0;
  const float cx = (box->x0 + box->x1) / 2.0f;
  const float cy = (box->y0 + box->y1) / 2.0f;
  float side = MAX(bw, bh) * (1.0f + 2.0f * DET_PAD);
  side = CLAMP(side, 32.0f, (float)MIN(w, h));
  r.side = (int)side;
  r.x = CLAMP((int)(cx - side / 2.0f), 0, w - r.side);
  r.y = CLAMP((int)(cy - side / 2.0f), 0, h - r.side);
  return r;
}

// resize a square region of the RGBA thumbnail into the classifier's
// float CHW tensor, RGB, 0..255
static void _prepare_input(const uint8_t *rgba, const int w, const int h,
                           const dt_ibis_rect_t rect, const int in_side,
                           const gboolean clip_norm, float *out)
{
  const int side = rect.side;
  const int ox = rect.x;
  const int oy = rect.y;
  const float scale = (float)side / (float)in_side;
  const int plane = in_side * in_side;

  for(int y = 0; y < in_side; y++)
  {
    const float sy = oy + (y + 0.5f) * scale - 0.5f;
    for(int x = 0; x < in_side; x++)
    {
      const float sx = ox + (x + 0.5f) * scale - 0.5f;
      float rgb[3];
      _sample(rgba, w, h, sx, sy, rgb);
      for(int c = 0; c < 3; c++)
        out[c * plane + y * in_side + x] = clip_norm
          ? (rgb[c] / 255.0f - CLIP_MEAN[c]) / CLIP_STD[c]
          : rgb[c];
    }
  }
}

// buffers and contexts one job carries from frame to frame
typedef struct dt_ibis_models_t
{
  dt_ai_context_t *clf;
  dt_ai_context_t *det;      // NULL when no detector model is present
  float *clf_in, *clf_out;
  float *det_in, *det_out;
  int n_out;                 // classifier outputs (probabilities, or embedding dim)
  int det_ch, det_anchors;   // detector output layout
  // embedding models (arch bioclip)
  gboolean embedding;
  int clf_side;              // input side: 300 for the fixed head, 224 for CLIP
  float *text_embeds;        // n_labels x n_out, L2-normalised rows
  float *probs;              // n_labels, filled per frame
  float logit_scale;
  int n_labels;
  const guint8 *allowed;     // n_labels mask from the region list, or NULL
} dt_ibis_models_t;

// returns the winning label index, or -1; *score gets its probability,
// *det_conf the detector's confidence (0 when no box was used); top[] and
// top_score[] get the N_CANDIDATES best (index -1 past the end)
static int _classify(dt_ibis_models_t *m, const dt_imgid_t imgid, float *score, float *det_conf,
                     int *top, float *top_score)
{
  for(int k = 0; k < N_CANDIDATES; k++) { top[k] = -1; top_score[k] = 0.0f; }
  *det_conf = 0.0f;
  dt_mipmap_buffer_t buf;
  // a bird can be a few percent of the frame; the detector needs pixels
  const dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(2 * DET_SIDE, 2 * DET_SIDE);
  dt_mipmap_cache_get(&buf, imgid, mip, DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf || buf.width < 8 || buf.height < 8)
  {
    if(buf.buf) dt_mipmap_cache_release(&buf);
    return -1;
  }

  dt_ibis_box_t box;
  gboolean found = FALSE;
  if(m->det)
    found = _detect(m->det, buf.buf, buf.width, buf.height, m->det_in, m->det_out,
                    m->det_ch, m->det_anchors, &box);
  if(found) *det_conf = box.conf;
  const dt_ibis_rect_t rect = _crop_rect(buf.width, buf.height, found ? &box : NULL);
  _prepare_input(buf.buf, buf.width, buf.height, rect, m->clf_side, m->embedding, m->clf_in);
  dt_mipmap_cache_release(&buf);

  int64_t in_shape[4] = { 1, 3, m->clf_side, m->clf_side };
  int64_t out_shape[2] = { 1, m->n_out };
  dt_ai_tensor_t in = { .data = m->clf_in, .type = DT_AI_FLOAT, .shape = in_shape, .ndim = 4 };
  dt_ai_tensor_t out = { .data = m->clf_out, .type = DT_AI_FLOAT, .shape = out_shape, .ndim = 2 };
  const double t0 = dt_get_wtime();
  if(dt_ai_run(m->clf, &in, 1, &out, 1) != 0)
  {
    dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] classifier run failed for image %d", imgid);
    return -1;
  }
  dt_print(DT_DEBUG_AI, "[ibis_identify] classifier %.3fs", dt_get_wtime() - t0);

  float *probs = m->clf_out;
  int n = m->n_out;
  if(m->embedding)
  {
    // cosine similarity against every species, scaled and softmaxed;
    // species outside the region mask are left out of the softmax so
    // the probability is "among the birds that occur here"
    const int d = m->n_out;
    float norm = 0.0f;
    for(int k = 0; k < d; k++) norm += m->clf_out[k] * m->clf_out[k];
    norm = sqrtf(MAX(norm, 1e-12f));
    float maxl = -1e30f;
    for(int i = 0; i < m->n_labels; i++)
    {
      if(m->allowed && !m->allowed[i]) { m->probs[i] = -1e30f; continue; }
      const float *t = m->text_embeds + (size_t)i * d;
      float dot = 0.0f;
      for(int k = 0; k < d; k++) dot += m->clf_out[k] * t[k];
      m->probs[i] = m->logit_scale * dot / norm;
      maxl = MAX(maxl, m->probs[i]);
    }
    float sum = 0.0f;
    for(int i = 0; i < m->n_labels; i++)
    {
      m->probs[i] = (m->probs[i] <= -1e29f) ? 0.0f : expf(m->probs[i] - maxl);
      sum += m->probs[i];
    }
    for(int i = 0; i < m->n_labels; i++) m->probs[i] /= MAX(sum, 1e-12f);
    probs = m->probs;
    n = m->n_labels;
  }
  else if(m->allowed)
  {
    for(int i = 0; i < n; i++)
      if(!m->allowed[i]) probs[i] = 0.0f;
  }

  // the N_CANDIDATES largest, by insertion
  for(int i = 0; i < n; i++)
  {
    const float v = probs[i];
    if(v <= top_score[N_CANDIDATES - 1] && top[N_CANDIDATES - 1] >= 0) continue;
    int k = N_CANDIDATES - 1;
    while(k > 0 && (top[k - 1] < 0 || v > top_score[k - 1]))
    {
      top[k] = top[k - 1];
      top_score[k] = top_score[k - 1];
      k--;
    }
    top[k] = i;
    top_score[k] = v;
  }
  if(top[0] < 0) return -1;
  *score = top_score[0];
  return top[0];
}

// replace the frame's candidate tags with this run's runners-up
static void _store_candidates(const dt_imgid_t imgid, char **labels, const int n_labels,
                              const int *top, const float *top_score)
{
  GList *old = NULL;
  dt_tag_get_attached(imgid, &old, FALSE);
  for(GList *t = old; t; t = g_list_next(t))
  {
    const dt_tag_t *tag = t->data;
    if(tag->tag && g_str_has_prefix(tag->tag, CAND_ROOT))
      dt_tag_detach(tag->id, imgid, FALSE, FALSE);
  }
  dt_tag_free_result(&old);
  for(int k = 0; k < N_CANDIDATES; k++)
  {
    if(top[k] < 0 || top[k] >= n_labels) break;
    char *name = g_strdup_printf("%s%d|%s|%d", CAND_ROOT, k + 1, labels[top[k]], (int)lroundf(top_score[k] * 100.0f));
    guint id = 0;
    if(dt_tag_new(name, &id)) dt_tag_attach(id, imgid, FALSE, FALSE);
    g_free(name);
  }
}

static void _review_update(dt_lib_module_t *self);

// --- the job --------------------------------------------------------------

static gboolean _job_finished_idle(gpointer data)
{
  dt_ibis_job_t *j = data;
  dt_lib_ibis_identify_t *d = j->self ? j->self->data : NULL;

  if(j->error)
  {
    dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] %s", j->error);
    dt_control_log(_("identify birds: %s"), j->error);
    if(d)
    {
      gtk_label_set_text(GTK_LABEL(d->status), j->error);
      // the fix for a missing key or model lives in settings: open them
      dt_conf_set_bool("plugins/lighttable/ibis_identify/settings_expanded", TRUE);
      dt_gui_update_collapsible_section(&d->settings);
    }
  }
  else
  {
    char *msg = g_strdup_printf(
      ngettext("%d image tagged", "%d images tagged", j->tagged), j->tagged);
    char *more = NULL;
    if(j->unsure || j->failed)
      more = g_strdup_printf(_("%s, %d unsure, %d could not be read"), msg, j->unsure, j->failed);
    dt_control_log("%s", more ? more : msg);
    if(d) gtk_label_set_text(GTK_LABEL(d->status), more ? more : msg);
    g_free(more);
    g_free(msg);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  }
  if(d)
  {
    gtk_widget_set_sensitive(d->run_button, TRUE);
    if(j->region_note) gtk_label_set_text(GTK_LABEL(d->region_label), j->region_note);
    _review_update(j->self);
  }

  g_list_free(j->images);
  g_free(j->model_id);
  g_free(j->detector_id);
  g_free(j->api_key);
  g_free(j->country);
  g_free(j->region_note);
  g_free(j->error);
  g_free(j);
  return G_SOURCE_REMOVE;
}

static int32_t _job_run(dt_job_t *job)
{
  dt_ibis_job_t *j = dt_control_job_get_params(job);
  dt_control_job_set_progress_message(job, _("loading bird classifier..."));

  dt_ai_environment_t *env = dt_ai_env_init(NULL);
  if(!env)
  {
    j->error = g_strdup(_("AI is disabled in preferences (processing > AI)"));
    goto done;
  }
  if(!dt_ai_get_model_info_by_id(env, j->model_id))
  {
    j->error = g_strdup_printf(_("model '%s' not found in the AI models folder"), j->model_id);
    dt_ai_env_destroy(env);
    goto done;
  }

  int n_labels = 0;
  char **labels = _load_labels(j->model_id, &n_labels);
  if(!labels || n_labels == 0)
  {
    j->error = g_strdup_printf(_("model '%s' has no labels.txt"), j->model_id);
    dt_ai_env_destroy(env);
    goto done;
  }

  const double t_load = dt_get_wtime();
  dt_ai_context_t *ctx = dt_ai_load_model(env, j->model_id, NULL, DT_AI_PROVIDER_CONFIGURED);
  dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] model %s %s in %.1fs", j->model_id, ctx ? "loaded" : "FAILED", dt_get_wtime() - t_load);
  if(!ctx)
  {
    j->error = g_strdup_printf(_("model '%s' failed to load"), j->model_id);
    g_strfreev(labels);
    dt_ai_env_destroy(env);
    goto done;
  }

  dt_ibis_models_t m = { .clf = ctx, .clf_side = INPUT_SIDE, .n_labels = n_labels };
  int64_t out_shape[8] = { 0 };
  const int out_ndim = dt_ai_get_output_shape(ctx, 0, out_shape, 8);
  m.n_out = out_ndim > 0 ? (int)out_shape[out_ndim - 1] : n_labels;
  if(m.n_out <= 0) m.n_out = n_labels;

  const dt_ai_model_info_t *info = dt_ai_get_model_info_by_id(env, j->model_id);
  if(info && info->arch && !strcmp(info->arch, "bioclip"))
  {
    m.embedding = TRUE;
    m.clf_side = dt_ai_model_attribute_int(info, "input_size", CLIP_SIDE);
    m.logit_scale = (float)dt_ai_model_attribute_double(info, "logit_scale", 100.0);
    const int dim = dt_ai_model_attribute_int(info, "embed_dim", m.n_out);
    if(dim != m.n_out)
      dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] embedding dim %d in manifest, %d from model", dim, m.n_out);
    char *file = dt_ai_model_attribute_string(info, "embeddings");
    gsize len = 0;
    gchar *raw = NULL;
    char *path = _model_file(j->model_id, file && file[0] ? file : "text_embeds.bin");
    if(!g_file_get_contents(path, &raw, &len, NULL) || len != (gsize)n_labels * m.n_out * sizeof(float))
    {
      j->error = g_strdup_printf(_("model '%s': text embeddings missing or not %d x %d floats"),
                                 j->model_id, n_labels, m.n_out);
      g_free(raw); g_free(path); g_free(file);
      g_strfreev(labels);
      dt_ai_unload_model(ctx);
      dt_ai_env_destroy(env);
      goto done;
    }
    m.text_embeds = dt_alloc_align_float((size_t)n_labels * m.n_out);
    memcpy(m.text_embeds, raw, len);
    m.probs = dt_alloc_align_float((size_t)n_labels);
    g_free(raw); g_free(path); g_free(file);
  }
  else if(m.n_out != n_labels)
    dt_print(DT_DEBUG_ALWAYS,
             "[ibis_identify] model has %d outputs but labels.txt has %d lines",
             m.n_out, n_labels);
  m.clf_in = dt_alloc_align_float((size_t)3 * m.clf_side * m.clf_side);
  m.clf_out = dt_alloc_align_float((size_t)m.n_out);

  // the region list: species names that occur where the frames were
  // taken. an explicit file wins; otherwise the frames' position (or the
  // country in the settings) asks eBird, with the answer cached a month
  guint8 *allowed = _load_region_mask(labels, n_labels);
  if(allowed)
    j->region_note = g_strdup(_("species list from file"));
  else if(j->auto_region)
  {
    if(!j->api_key || !j->api_key[0])
      j->region_note = g_strdup(_("no eBird API key: all species considered"));
    else
    {
      dt_control_job_set_progress_message(job, _("asking eBird which birds occur here..."));
      double lat = 0.0, lon = 0.0;
      char *region = NULL, *place = NULL;
      if(_median_position(j->images, &lat, &lon))
        region = _region_for_position(lat, lon, j->api_key, &place);
      else if(j->country && j->country[0])
        region = g_ascii_strup(j->country, -1);
      if(!region)
        j->region_note = g_strdup(_("no position on the frames and no country set: all species considered"));
      else
      {
        GHashTable *codes = _species_codes_for_region(region, j->api_key);
        int hits = 0;
        if(codes) allowed = _mask_from_codes(codes, j->model_id, labels, n_labels, &hits);
        if(allowed)
          j->region_note = place && place[0]
            ? g_strdup_printf(_("%s (%s): %d species"), place, region, hits)
            : g_strdup_printf(_("%s: %d species"), region, hits);
        else
          j->region_note = g_strdup_printf(_("eBird gave no species list for %s: all species considered"), region);
        if(codes) g_hash_table_destroy(codes);
      }
      g_free(region);
      g_free(place);
    }
  }
  else
    j->region_note = g_strdup(_("all species considered"));
  dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] candidates: %s", j->region_note);
  m.allowed = allowed;

  // the detector is optional: without it the frame's center is classified
  if(j->detector_id && j->detector_id[0] && dt_ai_get_model_info_by_id(env, j->detector_id))
  {
    m.det = dt_ai_load_model(env, j->detector_id, NULL, DT_AI_PROVIDER_CONFIGURED);
    int64_t det_shape[8] = { 0 };
    const int det_ndim = m.det ? dt_ai_get_output_shape(m.det, 0, det_shape, 8) : 0;
    if(det_ndim == 3 && det_shape[1] >= 5 && det_shape[2] > 0)
    {
      m.det_ch = (int)det_shape[1];
      m.det_anchors = (int)det_shape[2];
      m.det_in = dt_alloc_align_float((size_t)3 * DET_SIDE * DET_SIDE);
      m.det_out = dt_alloc_align_float((size_t)m.det_ch * m.det_anchors);
    }
    else
    {
      dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] detector '%s' unusable (%d dims); classifying whole frames",
               j->detector_id, det_ndim);
      if(m.det) dt_ai_unload_model(m.det);
      m.det = NULL;
    }
  }
  else
    dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] no detector model '%s'; classifying whole frames",
             j->detector_id ? j->detector_id : "");

  guint unknown_tag = 0;
  const int total = g_list_length(j->images);
  int count = 0;
  for(GList *l = j->images; l; l = g_list_next(l))
  {
    if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) break;
    const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
    float score = 0.0f, det_conf = 0.0f;
    int top[N_CANDIDATES];
    float top_score[N_CANDIDATES];
    const int best = _classify(&m, imgid, &score, &det_conf, top, top_score);
    if(best >= 0) _store_candidates(imgid, labels, n_labels, top, top_score);
    if(best < 0)
    {
      dt_print(DT_DEBUG_ALWAYS, "[ibis_identify] image %d: no result (thumbnail or model run failed)", imgid);
      j->failed++;
    }
    else if(score >= j->min_score && best < n_labels && labels[best][0])
    {
      char *tagname = g_strconcat(TAG_ROOT, labels[best], NULL);
      guint tagid = 0;
      // any earlier species guess on this frame is replaced, not stacked
      GList *old = NULL;
      dt_tag_get_attached(imgid, &old, TRUE);
      for(GList *t = old; t; t = g_list_next(t))
      {
        const dt_tag_t *tag = t->data;
        if(tag->tag && (g_str_has_prefix(tag->tag, TAG_ROOT) || !strcmp(tag->tag, TAG_UNKNOWN)))
          dt_tag_detach(tag->id, imgid, FALSE, FALSE);
      }
      dt_tag_free_result(&old);
      if(dt_tag_new(tagname, &tagid))
        dt_tag_attach(tagid, imgid, FALSE, FALSE);
      dt_print(DT_DEBUG_AI, "[ibis_identify] image %d: %s (%.3f, box %.2f)", imgid, labels[best], score, det_conf);
      g_free(tagname);
      j->tagged++;
    }
    else
    {
      // unsure: a species someone (or an earlier run) already put on the
      // frame outranks a weak guess, so it stays and nothing is added.
      // only a frame with no species at all gets the Unidentified marker
      gboolean has_species = FALSE;
      GList *old = NULL;
      dt_tag_get_attached(imgid, &old, TRUE);
      for(GList *t = old; t && !has_species; t = g_list_next(t))
      {
        const dt_tag_t *tag = t->data;
        has_species = tag->tag && g_str_has_prefix(tag->tag, TAG_ROOT)
                      && strcmp(tag->tag, TAG_UNKNOWN) != 0;
      }
      dt_tag_free_result(&old);
      if(!has_species)
      {
        if(!unknown_tag) dt_tag_new(TAG_UNKNOWN, &unknown_tag);
        if(unknown_tag) dt_tag_attach(unknown_tag, imgid, FALSE, FALSE);
      }
      dt_print(DT_DEBUG_AI, "[ibis_identify] image %d: unsure, best %s (%.3f, box %.2f)%s",
               imgid, best < n_labels ? labels[best] : "?", score, det_conf,
               has_species ? ", kept existing species" : "");
      j->unsure++;
    }
    count++;
    dt_control_job_set_progress(job, (double)count / (double)MAX(total, 1));
  }

  dt_free_align(m.clf_in);
  dt_free_align(m.clf_out);
  dt_free_align(m.det_in);
  dt_free_align(m.det_out);
  dt_free_align(m.text_embeds);
  dt_free_align(m.probs);
  g_free(allowed);
  if(m.det) dt_ai_unload_model(m.det);
  g_strfreev(labels);
  dt_ai_unload_model(ctx);
  dt_ai_env_destroy(env);

done:
  g_idle_add(_job_finished_idle, j);
  return 0;
}

// --- gui ------------------------------------------------------------------

static void _run_clicked(GtkWidget *w, dt_lib_module_t *self)
{
  dt_lib_ibis_identify_t *d = self->data;
  GList *images = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!images)
  {
    dt_control_log(_("identify birds: select some images first"));
    return;
  }

  dt_ibis_job_t *j = g_new0(dt_ibis_job_t, 1);
  j->images = images;
  j->min_score = dt_bauhaus_slider_get(d->min_score);
  j->model_id = dt_conf_get_string(CONF_MODEL_ID);
  if(!j->model_id || !j->model_id[0])
  {
    g_free(j->model_id);
    j->model_id = g_strdup("classify-birds");
  }
  j->detector_id = dt_conf_get_string(CONF_DETECTOR_ID);
  j->api_key = dt_conf_get_string(CONF_API_KEY);
  j->country = dt_conf_get_string(CONF_COUNTRY);
  j->auto_region = dt_conf_get_bool(CONF_AUTO_REGION);
  j->self = self;

  gtk_widget_set_sensitive(d->run_button, FALSE);
  gtk_label_set_text(GTK_LABEL(d->status), _("identifying..."));

  dt_job_t *job = dt_control_job_create(_job_run, "identify birds");
  dt_control_job_set_params(job, j, NULL); // freed by the idle callback
  dt_control_job_add_progress(job, _("identify birds"), TRUE);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
}

static void _min_score_changed(GtkWidget *w, gpointer data)
{
  dt_conf_set_float(CONF_MIN_SCORE, dt_bauhaus_slider_get(w));
}

static void _api_key_changed(GtkEntry *e, gpointer data)
{
  dt_conf_set_string(CONF_API_KEY, gtk_entry_get_text(e));
}

static void _country_changed(GtkEntry *e, gpointer data)
{
  dt_conf_set_string(CONF_COUNTRY, gtk_entry_get_text(e));
}

// --- review -----------------------------------------------------------------
//
// the hovered frame (else the one acted on) shows its species and the
// classifier's runners-up as buttons; a click makes that the species.
// this is how a wrong call is fixed without typing a name

static void _set_species(const dt_imgid_t imgid, const char *species)
{
  GList *old = NULL;
  dt_tag_get_attached(imgid, &old, TRUE);
  for(GList *t = old; t; t = g_list_next(t))
  {
    const dt_tag_t *tag = t->data;
    if(tag->tag && g_str_has_prefix(tag->tag, TAG_ROOT))
      dt_tag_detach(tag->id, imgid, TRUE, FALSE);
  }
  dt_tag_free_result(&old);
  if(species && species[0])
  {
    char *name = g_strconcat(TAG_ROOT, species, NULL);
    guint id = 0;
    if(dt_tag_new(name, &id)) dt_tag_attach(id, imgid, TRUE, FALSE);
    g_free(name);
  }
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
}

static void _candidate_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_ibis_identify_t *d = self->data;
  const char *species = g_object_get_data(G_OBJECT(b), "ibis-species");
  if(!dt_is_valid_imgid(d->review_imgid) || !species) return;
  _set_species(d->review_imgid, species);
  _review_update(self);
}

static void _no_bird_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_ibis_identify_t *d = self->data;
  if(!dt_is_valid_imgid(d->review_imgid)) return;
  _set_species(d->review_imgid, NULL);
  GList *old = NULL;
  dt_tag_get_attached(d->review_imgid, &old, FALSE);
  for(GList *t = old; t; t = g_list_next(t))
  {
    const dt_tag_t *tag = t->data;
    if(tag->tag && g_str_has_prefix(tag->tag, CAND_ROOT))
      dt_tag_detach(tag->id, d->review_imgid, FALSE, FALSE);
  }
  dt_tag_free_result(&old);
  _review_update(self);
}

static void _review_update(dt_lib_module_t *self)
{
  dt_lib_ibis_identify_t *d = self->data;
  if(!d || !d->review_title) return;
  dt_imgid_t imgid = dt_control_get_mouse_over_id();
  if(!dt_is_valid_imgid(imgid)) imgid = dt_act_on_get_main_image();
  d->review_imgid = imgid;

  for(int k = 0; k < N_CANDIDATES; k++) gtk_widget_hide(d->candidate[k]);
  if(!dt_is_valid_imgid(imgid))
  {
    gtk_label_set_text(GTK_LABEL(d->review_title), _("hover or select a frame to review"));
    gtk_widget_hide(d->no_bird);
    return;
  }

  char *species = NULL;
  char *cand[N_CANDIDATES] = { NULL };
  int pct[N_CANDIDATES] = { 0 };
  GList *tags = NULL;
  dt_tag_get_attached(imgid, &tags, FALSE);
  for(GList *t = tags; t; t = g_list_next(t))
  {
    const dt_tag_t *tag = t->data;
    if(!tag->tag) continue;
    if(g_str_has_prefix(tag->tag, TAG_ROOT) && !species)
      species = g_strdup(tag->tag + strlen(TAG_ROOT));
    else if(g_str_has_prefix(tag->tag, CAND_ROOT))
    {
      char **f = g_strsplit(tag->tag + strlen(CAND_ROOT), "|", 3);
      const int rank = f[0] ? atoi(f[0]) : 0;
      if(rank >= 1 && rank <= N_CANDIDATES && f[1])
      {
        g_free(cand[rank - 1]);
        cand[rank - 1] = g_strdup(f[1]);
        pct[rank - 1] = f[2] ? atoi(f[2]) : 0;
      }
      g_strfreev(f);
    }
  }
  dt_tag_free_result(&tags);

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  char *title = g_strdup_printf("%s: %s", img ? img->filename : "?",
                                species ? species : _("no species"));
  if(img) dt_image_cache_read_release(img);
  gtk_label_set_text(GTK_LABEL(d->review_title), title);
  g_free(title);

  gboolean any = FALSE;
  for(int k = 0; k < N_CANDIDATES; k++)
  {
    if(!cand[k]) continue;
    any = TRUE;
    const gboolean current = species && !strcmp(species, cand[k]);
    char *name = g_strdup_printf("%s%s", current ? "\xe2\x9c\x93  " : "", cand[k]);
    char *pcts = pct[k] < 1 ? g_strdup("<1%") : g_strdup_printf("%d%%", pct[k]);
    gtk_label_set_text(GTK_LABEL(d->candidate_name[k]), name);
    gtk_label_set_text(GTK_LABEL(d->candidate_pct[k]), pcts);
    g_free(name);
    g_free(pcts);
    g_object_set_data_full(G_OBJECT(d->candidate[k]), "ibis-species", g_strdup(cand[k]), g_free);
    gtk_widget_show(d->candidate[k]);
  }
  gtk_widget_set_visible(d->no_bird, any || species != NULL);
  for(int k = 0; k < N_CANDIDATES; k++) g_free(cand[k]);
  g_free(species);
}

static void _review_signal(gpointer instance, dt_lib_module_t *self)
{
  _review_update(self);
}

// a labeled row for the settings section: caption left, control right,
// so a masked key or a two-letter code never sits on screen unexplained
static GtkWidget *_labeled_row(GtkGrid *grid, const int row, const char *caption, GtkWidget *control)
{
  GtkWidget *l = gtk_label_new(caption);
  gtk_widget_set_halign(l, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
  gtk_grid_attach(grid, l, 0, row, 1, 1);
  gtk_widget_set_hexpand(control, TRUE);
  gtk_grid_attach(grid, control, 1, row, 1, 1);
  return control;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_ibis_identify_t *d = g_new0(dt_lib_ibis_identify_t, 1);
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // 1. act
  d->run_button = dt_action_button_new(self, N_("identify selected"), _run_clicked, self,
    _("run the bird classifier on the selected images\n"
      "and tag each one Birds|Species|<name>"), 0, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->run_button, FALSE, FALSE, 0);

  // 2. what happened, and which birds were candidates: one line each, never empty
  d->status = gtk_label_new(_("select frames, then identify"));
  gtk_label_set_ellipsize(GTK_LABEL(d->status), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(d->status, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(d->status), 0.0f);
  gtk_box_pack_start(GTK_BOX(self->widget), d->status, FALSE, FALSE, DT_PIXEL_APPLY_DPI(2));

  d->region_label = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(d->region_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(d->region_label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(d->region_label), 0.0f);
  gtk_widget_set_tooltip_text(d->region_label,
    _("which birds were candidates: the species eBird lists for the\n"
      "region of the frames' position (or your country), or every species"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->region_label, FALSE, FALSE, 0);

  // 3. check the answer: the hovered (else selected) frame and its runners-up
  d->review_title = gtk_label_new(_("hover or select a frame to review"));
  gtk_label_set_ellipsize(GTK_LABEL(d->review_title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(d->review_title, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(d->review_title), 0.0f);
  gtk_box_pack_start(GTK_BOX(self->widget), d->review_title, FALSE, FALSE, DT_PIXEL_APPLY_DPI(6));
  for(int k = 0; k < N_CANDIDATES; k++)
  {
    // species left, percentage right, like a ledger line
    d->candidate[k] = gtk_button_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
    d->candidate_name[k] = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->candidate_name[k]), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(d->candidate_name[k]), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(d->candidate_name[k], TRUE);
    d->candidate_pct[k] = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(d->candidate_pct[k]), 1.0f);
    gtk_box_pack_start(GTK_BOX(row), d->candidate_name[k], TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row), d->candidate_pct[k], FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(d->candidate[k]), row);
    gtk_widget_show_all(row);
    gtk_widget_set_tooltip_text(d->candidate[k], _("make this the frame's species"));
    g_signal_connect(d->candidate[k], "clicked", G_CALLBACK(_candidate_clicked), self);
    gtk_box_pack_start(GTK_BOX(self->widget), d->candidate[k], FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(d->candidate[k], TRUE);
  }
  d->no_bird = gtk_button_new_with_label(_("no bird here"));
  gtk_button_set_relief(GTK_BUTTON(d->no_bird), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(d->no_bird, _("remove the species and the candidates from the frame"));
  g_signal_connect(d->no_bird, "clicked", G_CALLBACK(_no_bird_clicked), self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->no_bird, FALSE, FALSE, 0);
  gtk_widget_set_no_show_all(d->no_bird, TRUE);

  // 4. settings, folded away: set once, rarely touched
  dt_gui_new_collapsible_section(&d->settings, "plugins/lighttable/ibis_identify/settings_expanded",
                                 _("settings"), GTK_BOX(self->widget), DT_ACTION(self));
  gtk_widget_set_tooltip_text(d->settings.expander,
    _("minimum score, eBird key and fallback country"));

  d->min_score = dt_bauhaus_slider_new_action(self, 0.0f, 1.0f, 0, 0.5f, 2);
  dt_bauhaus_widget_set_label(d->min_score, NULL, N_("minimum score"));
  dt_bauhaus_slider_set(d->min_score, dt_conf_get_float(CONF_MIN_SCORE));
  gtk_widget_set_tooltip_text(d->min_score,
    _("below this the frame is tagged Birds|Species|Unidentified\n"
      "instead of a species, so nothing is skipped silently"));
  g_signal_connect(d->min_score, "value-changed", G_CALLBACK(_min_score_changed), NULL);
  gtk_box_pack_start(d->settings.container, d->min_score, FALSE, FALSE, 0);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(8));
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(2));

  d->api_key = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(d->api_key), FALSE);
  char *key = dt_conf_get_string(CONF_API_KEY);
  gtk_entry_set_text(GTK_ENTRY(d->api_key), key ? key : "");
  g_free(key);
  g_signal_connect(d->api_key, "changed", G_CALLBACK(_api_key_changed), NULL);
  gtk_widget_set_tooltip_text(d->api_key,
    _("lets identify birds ask eBird which species occur where the frames\n"
      "were taken, so only those are candidates. free at ebird.org/api/keygen"));
  _labeled_row(GTK_GRID(grid), 0, _("eBird API key"), d->api_key);

  d->country = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(d->country), 2);
  gtk_entry_set_width_chars(GTK_ENTRY(d->country), 4);
  char *cc = dt_conf_get_string(CONF_COUNTRY);
  gtk_entry_set_text(GTK_ENTRY(d->country), cc ? cc : "");
  g_free(cc);
  g_signal_connect(d->country, "changed", G_CALLBACK(_country_changed), NULL);
  gtk_widget_set_tooltip_text(d->country,
    _("two-letter country code used as the region when no frame has a position (NO, NL, US)"));
  _labeled_row(GTK_GRID(grid), 1, _("fallback country"), d->country);

  gtk_box_pack_start(d->settings.container, grid, FALSE, FALSE, DT_PIXEL_APPLY_DPI(2));

  d->review_imgid = NO_IMGID;
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE, _review_signal);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_SELECTION_CHANGED, _review_signal);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_TAG_CHANGED, _review_signal);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_CONTROL_SIGNAL_DISCONNECT_ALL(self, "ibis_identify");
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
