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
// input contract of the bundled model (Kestrel species head v1):
//   'data' float32 NCHW 1x3x300x300, RGB, 0..255, not normalised
//   'model_output' float32 1xN softmax probabilities
// the frame is center-cropped to a square before resizing: the bird is
// almost always where the photographer put it, and stretching a 3:2
// frame to a square costs more than the edges do.
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

#define CONF_MIN_SCORE "plugins/lighttable/ibis_identify/min_score"
#define CONF_MODEL_ID  "plugins/lighttable/ibis_identify/model_id"

#define TAG_ROOT      "Birds|Species|"
#define TAG_UNKNOWN   "Birds|Species|Unidentified"
#define INPUT_SIDE    300
#define MAX_LABELS    4096

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

// center-crop the RGBA thumbnail to a square and bilinearly resize it
// into a float CHW tensor, RGB, 0..255
static void _prepare_input(const uint8_t *rgba, const int w, const int h, float *out)
{
  const int side = MIN(w, h);
  const int ox = (w - side) / 2;
  const int oy = (h - side) / 2;
  const float scale = (float)side / (float)INPUT_SIDE;
  const int plane = INPUT_SIDE * INPUT_SIDE;

  for(int y = 0; y < INPUT_SIDE; y++)
  {
    const float sy = (y + 0.5f) * scale - 0.5f;
    const int y0 = CLAMP((int)floorf(sy), 0, side - 1);
    const int y1 = MIN(y0 + 1, side - 1);
    const float fy = CLAMP(sy - y0, 0.0f, 1.0f);
    for(int x = 0; x < INPUT_SIDE; x++)
    {
      const float sx = (x + 0.5f) * scale - 0.5f;
      const int x0 = CLAMP((int)floorf(sx), 0, side - 1);
      const int x1 = MIN(x0 + 1, side - 1);
      const float fx = CLAMP(sx - x0, 0.0f, 1.0f);
      const uint8_t *p00 = rgba + 4 * ((oy + y0) * w + ox + x0);
      const uint8_t *p01 = rgba + 4 * ((oy + y0) * w + ox + x1);
      const uint8_t *p10 = rgba + 4 * ((oy + y1) * w + ox + x0);
      const uint8_t *p11 = rgba + 4 * ((oy + y1) * w + ox + x1);
      for(int c = 0; c < 3; c++)
      {
        const float top = p00[c] * (1.0f - fx) + p01[c] * fx;
        const float bot = p10[c] * (1.0f - fx) + p11[c] * fx;
        out[c * plane + y * INPUT_SIDE + x] = top * (1.0f - fy) + bot * fy;
      }
    }
  }
}

// returns the winning label index, or -1; *score gets its probability
static int _classify(dt_ai_context_t *ctx, const dt_imgid_t imgid,
                     float *input, float *output, const int n_out, float *score)
{
  dt_mipmap_buffer_t buf;
  const dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(2 * INPUT_SIDE, 2 * INPUT_SIDE);
  dt_mipmap_cache_get(&buf, imgid, mip, DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf || buf.width < 8 || buf.height < 8)
  {
    if(buf.buf) dt_mipmap_cache_release(&buf);
    return -1;
  }
  _prepare_input(buf.buf, buf.width, buf.height, input);
  dt_mipmap_cache_release(&buf);

  int64_t in_shape[4] = { 1, 3, INPUT_SIDE, INPUT_SIDE };
  int64_t out_shape[2] = { 1, n_out };
  dt_ai_tensor_t in = { .data = input, .type = DT_AI_FLOAT, .shape = in_shape, .ndim = 4 };
  dt_ai_tensor_t out = { .data = output, .type = DT_AI_FLOAT, .shape = out_shape, .ndim = 2 };
  if(dt_ai_run(ctx, &in, 1, &out, 1) != 0)
    return -1;

  int best = 0;
  for(int i = 1; i < n_out; i++)
    if(output[i] > output[best]) best = i;
  *score = output[best];
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

  int64_t out_shape[8] = { 0 };
  const int out_ndim = dt_ai_get_output_shape(ctx, 0, out_shape, 8);
  int n_out = out_ndim > 0 ? (int)out_shape[out_ndim - 1] : n_labels;
  if(n_out <= 0) n_out = n_labels;
  if(n_out != n_labels)
    dt_print(DT_DEBUG_ALWAYS,
             "[ibis_identify] model has %d outputs but labels.txt has %d lines",
             n_out, n_labels);

  float *input = dt_alloc_align_float((size_t)3 * INPUT_SIDE * INPUT_SIDE);
  float *output = dt_alloc_align_float((size_t)n_out);

  guint unknown_tag = 0;
  const int total = g_list_length(j->images);
  int count = 0;
  for(GList *l = j->images; l; l = g_list_next(l))
  {
    if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) break;
    const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
    float score = 0.0f;
    const int best = _classify(ctx, imgid, input, output, n_out, &score);
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
      dt_print(DT_DEBUG_AI, "[ibis_identify] image %d: %s (%.3f)", imgid, labels[best], score);
      g_free(tagname);
      j->tagged++;
    }
    else
    {
      if(!unknown_tag) dt_tag_new(TAG_UNKNOWN, &unknown_tag);
      if(unknown_tag) dt_tag_attach(unknown_tag, imgid, FALSE, FALSE);
      dt_print(DT_DEBUG_AI, "[ibis_identify] image %d: unsure, best %s (%.3f)",
               imgid, best < n_labels ? labels[best] : "?", score);
      j->unsure++;
    }
    count++;
    dt_control_job_set_progress(job, (double)count / (double)MAX(total, 1));
  }

  dt_free_align(input);
  dt_free_align(output);
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
