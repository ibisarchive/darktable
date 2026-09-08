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
// input contract of the bundled classifier (Kestrel species head v1):
//   'data' float32 NCHW 1x3x300x300, RGB, 0..255, not normalised
//   'model_output' float32 1xN softmax probabilities
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

#include <math.h>
#include <string.h>

DT_MODULE(1)

#define CONF_MIN_SCORE   "plugins/lighttable/ibis_identify/min_score"
#define CONF_MODEL_ID    "plugins/lighttable/ibis_identify/model_id"
#define CONF_DETECTOR_ID "plugins/lighttable/ibis_identify/detector_id"

#define TAG_ROOT      "Birds|Species|"
#define TAG_UNKNOWN   "Birds|Species|Unidentified"
#define INPUT_SIDE    300
#define MAX_LABELS    4096

#define DET_SIDE      640
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

typedef struct dt_lib_ibis_identify_t
{
  GtkWidget *run_button;
  GtkWidget *min_score;
  GtkWidget *status;
} dt_lib_ibis_identify_t;

typedef struct dt_ibis_job_t
{
  GList *images;          // imgids to classify
  float min_score;
  char *model_id;
  char *detector_id;
  dt_lib_module_t *self;
  // results, filled by the job, read by the idle callback
  int tagged;
  int unsure;
  int failed;
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
  return 410; // just below neural restore
}

// --- labels ---------------------------------------------------------------

// labels.txt: one common name per line, UTF-8, optional BOM; the line
// index is the output column
static char **_load_labels(const char *model_id, int *count)
{
  *count = 0;
  char *dir = dt_ai_resolve_models_path_override();
  char *path = dir
    ? g_build_filename(dir, model_id, "labels.txt", NULL)
    : g_build_filename(g_get_user_data_dir(), "darktable", "models", model_id, "labels.txt", NULL);
  g_free(dir);

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
  char **lines = g_strsplit(start, "\n", MAX_LABELS);
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
                           const dt_ibis_rect_t rect, float *out)
{
  const int side = rect.side;
  const int ox = rect.x;
  const int oy = rect.y;
  const float scale = (float)side / (float)INPUT_SIDE;
  const int plane = INPUT_SIDE * INPUT_SIDE;

  for(int y = 0; y < INPUT_SIDE; y++)
  {
    const float sy = oy + (y + 0.5f) * scale - 0.5f;
    for(int x = 0; x < INPUT_SIDE; x++)
    {
      const float sx = ox + (x + 0.5f) * scale - 0.5f;
      float rgb[3];
      _sample(rgba, w, h, sx, sy, rgb);
      for(int c = 0; c < 3; c++)
        out[c * plane + y * INPUT_SIDE + x] = rgb[c];
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
  int n_out;                 // classifier outputs
  int det_ch, det_anchors;   // detector output layout
} dt_ibis_models_t;

// returns the winning label index, or -1; *score gets its probability,
// *det_conf the detector's confidence (0 when no box was used)
static int _classify(dt_ibis_models_t *m, const dt_imgid_t imgid, float *score, float *det_conf)
{
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
  _prepare_input(buf.buf, buf.width, buf.height, rect, m->clf_in);
  dt_mipmap_cache_release(&buf);

  int64_t in_shape[4] = { 1, 3, INPUT_SIDE, INPUT_SIDE };
  int64_t out_shape[2] = { 1, m->n_out };
  dt_ai_tensor_t in = { .data = m->clf_in, .type = DT_AI_FLOAT, .shape = in_shape, .ndim = 4 };
  dt_ai_tensor_t out = { .data = m->clf_out, .type = DT_AI_FLOAT, .shape = out_shape, .ndim = 2 };
  if(dt_ai_run(m->clf, &in, 1, &out, 1) != 0)
    return -1;

  int best = 0;
  for(int i = 1; i < m->n_out; i++)
    if(m->clf_out[i] > m->clf_out[best]) best = i;
  *score = m->clf_out[best];
  return best;
}

// --- the job --------------------------------------------------------------

static gboolean _job_finished_idle(gpointer data)
{
  dt_ibis_job_t *j = data;
  dt_lib_ibis_identify_t *d = j->self ? j->self->data : NULL;

  if(j->error)
  {
    dt_control_log(_("identify birds: %s"), j->error);
    if(d) gtk_label_set_text(GTK_LABEL(d->status), j->error);
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
  if(d) gtk_widget_set_sensitive(d->run_button, TRUE);

  g_list_free(j->images);
  g_free(j->model_id);
  g_free(j->detector_id);
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

  dt_ai_context_t *ctx = dt_ai_load_model(env, j->model_id, NULL, DT_AI_PROVIDER_CONFIGURED);
  if(!ctx)
  {
    j->error = g_strdup_printf(_("model '%s' failed to load"), j->model_id);
    g_strfreev(labels);
    dt_ai_env_destroy(env);
    goto done;
  }

  dt_ibis_models_t m = { .clf = ctx };
  int64_t out_shape[8] = { 0 };
  const int out_ndim = dt_ai_get_output_shape(ctx, 0, out_shape, 8);
  m.n_out = out_ndim > 0 ? (int)out_shape[out_ndim - 1] : n_labels;
  if(m.n_out <= 0) m.n_out = n_labels;
  if(m.n_out != n_labels)
    dt_print(DT_DEBUG_ALWAYS,
             "[ibis_identify] model has %d outputs but labels.txt has %d lines",
             m.n_out, n_labels);
  m.clf_in = dt_alloc_align_float((size_t)3 * INPUT_SIDE * INPUT_SIDE);
  m.clf_out = dt_alloc_align_float((size_t)m.n_out);

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
    const int best = _classify(&m, imgid, &score, &det_conf);
    if(best < 0)
    {
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

void gui_init(dt_lib_module_t *self)
{
  dt_lib_ibis_identify_t *d = g_new0(dt_lib_ibis_identify_t, 1);
  self->data = d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  d->min_score = dt_bauhaus_slider_new_action(self, 0.0f, 1.0f, 0, 0.5f, 2);
  dt_bauhaus_widget_set_label(d->min_score, NULL, N_("minimum score"));
  dt_bauhaus_slider_set(d->min_score, dt_conf_get_float(CONF_MIN_SCORE));
  gtk_widget_set_tooltip_text(d->min_score,
    _("below this the frame is tagged Birds|Species|Unidentified\n"
      "instead of a species, so nothing is skipped silently"));
  g_signal_connect(d->min_score, "value-changed", G_CALLBACK(_min_score_changed), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), d->min_score, FALSE, FALSE, 0);

  d->run_button = dt_action_button_new(self, N_("identify selected"), _run_clicked, self,
    _("run the bird classifier on the selected images\n"
      "and tag each one Birds|Species|<name>"), 0, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->run_button, FALSE, FALSE, 0);

  d->status = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(d->status), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(d->status, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(self->widget), d->status, FALSE, FALSE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
