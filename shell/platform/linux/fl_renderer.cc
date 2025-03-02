// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fl_renderer.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include "flutter/shell/platform/embedder/embedder.h"
#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_framebuffer.h"
#include "flutter/shell/platform/linux/fl_view_private.h"

// Vertex shader to draw Flutter window contents.
static const char* vertex_shader_src =
    "attribute vec2 position;\n"
    "attribute vec2 in_texcoord;\n"
    "varying vec2 texcoord;\n"
    "\n"
    "void main() {\n"
    "  gl_Position = vec4(position, 0, 1);\n"
    "  texcoord = in_texcoord;\n"
    "}\n";

// Fragment shader to draw Flutter window contents.
static const char* fragment_shader_src =
    "uniform sampler2D texture;\n"
    "varying vec2 texcoord;\n"
    "\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(texture, texcoord);\n"
    "}\n";

G_DEFINE_QUARK(fl_renderer_error_quark, fl_renderer_error)

typedef struct {
  // Engine we are rendering.
  GWeakRef engine;

  // Views being rendered.
  GHashTable* views;

  // target dimension for resizing
  int target_width;
  int target_height;

  // whether the renderer waits for frame render
  bool blocking_main_thread;

  // true if frame was completed; resizing is not synchronized until first frame
  // was rendered
  bool had_first_frame;

  // True if we can use glBlitFramebuffer.
  bool has_gl_framebuffer_blit;

  // Shader program.
  GLuint program;

  // Framebuffers to render.
  GPtrArray* framebuffers;
} FlRendererPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(FlRenderer, fl_renderer, G_TYPE_OBJECT)

// Check if running on an NVIDIA driver.
static gboolean is_nvidia() {
  const gchar* vendor = reinterpret_cast<const gchar*>(glGetString(GL_VENDOR));
  return strstr(vendor, "NVIDIA") != nullptr;
}

// Returns the log for the given OpenGL shader. Must be freed by the caller.
static gchar* get_shader_log(GLuint shader) {
  GLint log_length;
  gchar* log;

  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

  log = static_cast<gchar*>(g_malloc(log_length + 1));
  glGetShaderInfoLog(shader, log_length, nullptr, log);

  return log;
}

// Returns the log for the given OpenGL program. Must be freed by the caller.
static gchar* get_program_log(GLuint program) {
  GLint log_length;
  gchar* log;

  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);

  log = static_cast<gchar*>(g_malloc(log_length + 1));
  glGetProgramInfoLog(program, log_length, nullptr, log);

  return log;
}

/// Converts a pixel co-ordinate from 0..pixels to OpenGL -1..1.
static GLfloat pixels_to_gl_coords(GLfloat position, GLfloat pixels) {
  return (2.0 * position / pixels) - 1.0;
}

static void fl_renderer_unblock_main_thread(FlRenderer* self) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));
  if (priv->blocking_main_thread) {
    priv->blocking_main_thread = false;

    g_autoptr(FlEngine) engine = FL_ENGINE(g_weak_ref_get(&priv->engine));
    if (engine != nullptr) {
      fl_task_runner_release_main_thread(fl_engine_get_task_runner(engine));
    }
  }
}

static void setup_shader(FlRenderer* self) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertex_shader, 1, &vertex_shader_src, nullptr);
  glCompileShader(vertex_shader);
  GLint vertex_compile_status;
  glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &vertex_compile_status);
  if (vertex_compile_status == GL_FALSE) {
    g_autofree gchar* shader_log = get_shader_log(vertex_shader);
    g_warning("Failed to compile vertex shader: %s", shader_log);
  }

  GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment_shader, 1, &fragment_shader_src, nullptr);
  glCompileShader(fragment_shader);
  GLint fragment_compile_status;
  glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &fragment_compile_status);
  if (fragment_compile_status == GL_FALSE) {
    g_autofree gchar* shader_log = get_shader_log(fragment_shader);
    g_warning("Failed to compile fragment shader: %s", shader_log);
  }

  priv->program = glCreateProgram();
  glAttachShader(priv->program, vertex_shader);
  glAttachShader(priv->program, fragment_shader);
  glLinkProgram(priv->program);

  GLint link_status;
  glGetProgramiv(priv->program, GL_LINK_STATUS, &link_status);
  if (link_status == GL_FALSE) {
    g_autofree gchar* program_log = get_program_log(priv->program);
    g_warning("Failed to link program: %s", program_log);
  }

  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
}

static void render_with_blit(FlRenderer* self) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  // Disable the scissor test as it can affect blit operations.
  // Prevents regressions like: https://github.com/flutter/flutter/issues/140828
  // See OpenGL specification version 4.6, section 18.3.1.
  glDisable(GL_SCISSOR_TEST);

  for (guint i = 0; i < priv->framebuffers->len; i++) {
    FlFramebuffer* framebuffer =
        FL_FRAMEBUFFER(g_ptr_array_index(priv->framebuffers, i));

    GLuint framebuffer_id = fl_framebuffer_get_id(framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer_id);
    size_t width = fl_framebuffer_get_width(framebuffer);
    size_t height = fl_framebuffer_get_height(framebuffer);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

static void render_with_textures(FlRenderer* self, int width, int height) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  // Save bindings that are set by this function.  All bindings must be restored
  // to their original values because Skia expects that its bindings have not
  // been altered.
  GLint saved_texture_binding;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_texture_binding);
  GLint saved_vao_binding;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vao_binding);
  GLint saved_array_buffer_binding;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved_array_buffer_binding);

  glUseProgram(priv->program);

  for (guint i = 0; i < priv->framebuffers->len; i++) {
    FlFramebuffer* framebuffer =
        FL_FRAMEBUFFER(g_ptr_array_index(priv->framebuffers, i));

    GLuint texture_id = fl_framebuffer_get_texture_id(framebuffer);
    glBindTexture(GL_TEXTURE_2D, texture_id);

    // Translate into OpenGL co-ordinates
    size_t texture_width = fl_framebuffer_get_width(framebuffer);
    size_t texture_height = fl_framebuffer_get_height(framebuffer);
    GLfloat x0 = pixels_to_gl_coords(0, width);
    GLfloat y0 = pixels_to_gl_coords(height - texture_height, height);
    GLfloat x1 = pixels_to_gl_coords(texture_width, width);
    GLfloat y1 = pixels_to_gl_coords(height, height);
    GLfloat vertex_data[] = {x0, y0, 0, 0, x1, y1, 1, 1, x0, y1, 0, 1,
                             x0, y0, 0, 0, x1, y0, 1, 0, x1, y1, 1, 1};

    GLuint vao, vertex_buffer;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data,
                 GL_STATIC_DRAW);
    GLint position_index = glGetAttribLocation(priv->program, "position");
    glEnableVertexAttribArray(position_index);
    glVertexAttribPointer(position_index, 2, GL_FLOAT, GL_FALSE,
                          sizeof(GLfloat) * 4, 0);
    GLint texcoord_index = glGetAttribLocation(priv->program, "in_texcoord");
    glEnableVertexAttribArray(texcoord_index);
    glVertexAttribPointer(texcoord_index, 2, GL_FLOAT, GL_FALSE,
                          sizeof(GLfloat) * 4, (void*)(sizeof(GLfloat) * 2));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vertex_buffer);
  }

  glBindTexture(GL_TEXTURE_2D, saved_texture_binding);
  glBindVertexArray(saved_vao_binding);
  glBindBuffer(GL_ARRAY_BUFFER, saved_array_buffer_binding);
}

static void fl_renderer_dispose(GObject* object) {
  FlRenderer* self = FL_RENDERER(object);
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  fl_renderer_unblock_main_thread(self);

  g_weak_ref_clear(&priv->engine);
  g_clear_pointer(&priv->views, g_hash_table_unref);
  g_clear_pointer(&priv->framebuffers, g_ptr_array_unref);

  G_OBJECT_CLASS(fl_renderer_parent_class)->dispose(object);
}

static void fl_renderer_class_init(FlRendererClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_renderer_dispose;
}

static void fl_renderer_init(FlRenderer* self) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));
  priv->views =
      g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, nullptr);
  priv->framebuffers = g_ptr_array_new_with_free_func(g_object_unref);
}

void fl_renderer_set_engine(FlRenderer* self, FlEngine* engine) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  g_return_if_fail(FL_IS_RENDERER(self));

  g_weak_ref_init(&priv->engine, engine);
}

void fl_renderer_add_view(FlRenderer* self,
                          FlutterViewId view_id,
                          FlView* view) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  g_return_if_fail(FL_IS_RENDERER(self));

  g_hash_table_insert(priv->views, GINT_TO_POINTER(view_id), view);
}

void* fl_renderer_get_proc_address(FlRenderer* self, const char* name) {
  g_return_val_if_fail(FL_IS_RENDERER(self), NULL);

  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void fl_renderer_make_current(FlRenderer* self) {
  g_return_if_fail(FL_IS_RENDERER(self));
  FL_RENDERER_GET_CLASS(self)->make_current(self);
}

void fl_renderer_make_resource_current(FlRenderer* self) {
  g_return_if_fail(FL_IS_RENDERER(self));
  FL_RENDERER_GET_CLASS(self)->make_resource_current(self);
}

void fl_renderer_clear_current(FlRenderer* self) {
  g_return_if_fail(FL_IS_RENDERER(self));
  FL_RENDERER_GET_CLASS(self)->clear_current(self);
}

gdouble fl_renderer_get_refresh_rate(FlRenderer* self) {
  g_return_val_if_fail(FL_IS_RENDERER(self), -1.0);
  return FL_RENDERER_GET_CLASS(self)->get_refresh_rate(self);
}

guint32 fl_renderer_get_fbo(FlRenderer* self) {
  g_return_val_if_fail(FL_IS_RENDERER(self), 0);

  // There is only one frame buffer object - always return that.
  return 0;
}

gboolean fl_renderer_create_backing_store(
    FlRenderer* renderer,
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out) {
  fl_renderer_make_current(renderer);

  FlFramebuffer* framebuffer =
      fl_framebuffer_new(config->size.width, config->size.height);
  if (!framebuffer) {
    g_warning("Failed to create backing store");
    return FALSE;
  }

  backing_store_out->type = kFlutterBackingStoreTypeOpenGL;
  backing_store_out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  backing_store_out->open_gl.framebuffer.user_data = framebuffer;
  backing_store_out->open_gl.framebuffer.name =
      fl_framebuffer_get_id(framebuffer);
  backing_store_out->open_gl.framebuffer.target =
      fl_framebuffer_get_format(framebuffer);
  backing_store_out->open_gl.framebuffer.destruction_callback = [](void* p) {
    // Backing store destroyed in fl_renderer_collect_backing_store(), set
    // on FlutterCompositor.collect_backing_store_callback during engine start.
  };

  return TRUE;
}

gboolean fl_renderer_collect_backing_store(
    FlRenderer* self,
    const FlutterBackingStore* backing_store) {
  fl_renderer_make_current(self);

  // OpenGL context is required when destroying #FlFramebuffer.
  g_object_unref(backing_store->open_gl.framebuffer.user_data);
  return TRUE;
}

void fl_renderer_wait_for_frame(FlRenderer* self,
                                int target_width,
                                int target_height) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  g_return_if_fail(FL_IS_RENDERER(self));

  priv->target_width = target_width;
  priv->target_height = target_height;

  if (priv->had_first_frame && !priv->blocking_main_thread) {
    priv->blocking_main_thread = true;
    g_autoptr(FlEngine) engine = FL_ENGINE(g_weak_ref_get(&priv->engine));
    if (engine != nullptr) {
      fl_task_runner_block_main_thread(fl_engine_get_task_runner(engine));
    }
  }
}

gboolean fl_renderer_present_layers(FlRenderer* self,
                                    FlutterViewId view_id,
                                    const FlutterLayer** layers,
                                    size_t layers_count) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  g_return_val_if_fail(FL_IS_RENDERER(self), FALSE);

  // ignore incoming frame with wrong dimensions in trivial case with just one
  // layer
  if (priv->blocking_main_thread && layers_count == 1 &&
      layers[0]->offset.x == 0 && layers[0]->offset.y == 0 &&
      (layers[0]->size.width != priv->target_width ||
       layers[0]->size.height != priv->target_height)) {
    return true;
  }

  priv->had_first_frame = true;

  fl_renderer_unblock_main_thread(self);

  g_ptr_array_set_size(priv->framebuffers, 0);
  for (size_t i = 0; i < layers_count; ++i) {
    const FlutterLayer* layer = layers[i];
    switch (layer->type) {
      case kFlutterLayerContentTypeBackingStore: {
        const FlutterBackingStore* backing_store = layer->backing_store;
        FlFramebuffer* framebuffer =
            FL_FRAMEBUFFER(backing_store->open_gl.framebuffer.user_data);
        g_ptr_array_add(priv->framebuffers, g_object_ref(framebuffer));
      } break;
      case kFlutterLayerContentTypePlatformView: {
        // TODO(robert-ancell) Not implemented -
        // https://github.com/flutter/flutter/issues/41724
      } break;
    }
  }

  FlView* view =
      FL_VIEW(g_hash_table_lookup(priv->views, GINT_TO_POINTER(view_id)));
  if (view != nullptr) {
    fl_view_redraw(view);
  }

  return TRUE;
}

void fl_renderer_setup(FlRenderer* self) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  g_return_if_fail(FL_IS_RENDERER(self));

  // Note: NVIDIA is temporarily disabled due to
  // https://github.com/flutter/flutter/issues/152099
  priv->has_gl_framebuffer_blit =
      !is_nvidia() && (epoxy_gl_version() >= 30 ||
                       epoxy_has_gl_extension("GL_EXT_framebuffer_blit"));

  if (!priv->has_gl_framebuffer_blit) {
    setup_shader(self);
  }
}

void fl_renderer_render(FlRenderer* self, int width, int height) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  g_return_if_fail(FL_IS_RENDERER(self));

  glClearColor(0.0, 0.0, 0.0, 0.0);
  glClear(GL_COLOR_BUFFER_BIT);

  if (priv->has_gl_framebuffer_blit) {
    render_with_blit(self);
  } else {
    render_with_textures(self, width, height);
  }

  glFlush();
}

void fl_renderer_cleanup(FlRenderer* self) {
  FlRendererPrivate* priv = reinterpret_cast<FlRendererPrivate*>(
      fl_renderer_get_instance_private(self));

  g_return_if_fail(FL_IS_RENDERER(self));

  if (priv->program != 0) {
    glDeleteProgram(priv->program);
  }
}
