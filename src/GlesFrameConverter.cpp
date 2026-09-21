/*
 * Copyright 2026 FloralDroid
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "FloralCodec2Gles"

#include "floral/codec/GlesFrameConverter.h"

#include <C2AllocatorGralloc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <cutils/native_handle.h>
#include <drm_fourcc.h>
#include <log/log.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace floral::codec {
namespace {

using ImageTargetTextureProc = void (*)(GLenum target, void *image);

constexpr char kVertexShader[] = R"(
#version 300 es
void main() {
  const vec2 positions[3] = vec2[3](
      vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)";

constexpr char kLumaShader[] = R"(
#version 300 es
precision highp float;
precision highp int;
uniform sampler2D sourceTexture;
uniform ivec2 sourceSize;
layout(location = 0) out vec4 outputColor;

void main() {
  ivec2 position = clamp(ivec2(gl_FragCoord.xy), ivec2(0), sourceSize - 1);
  vec3 rgb = texelFetch(sourceTexture, position, 0).rgb;
  float y = dot(rgb, vec3(66.0, 129.0, 25.0)) / 256.0 + 16.0 / 255.0;
  outputColor = vec4(clamp(y, 0.0, 1.0), 0.0, 0.0, 1.0);
}
)";

constexpr char kChromaShader[] = R"(
#version 300 es
precision highp float;
precision highp int;
uniform sampler2D sourceTexture;
uniform ivec2 sourceSize;
layout(location = 0) out vec4 outputColor;

vec3 sampleRgb(ivec2 position) {
  return texelFetch(sourceTexture,
                    clamp(position, ivec2(0), sourceSize - 1), 0).rgb;
}

void main() {
  ivec2 position = ivec2(gl_FragCoord.xy) * 2;
  vec3 rgb = (sampleRgb(position) + sampleRgb(position + ivec2(1, 0)) +
              sampleRgb(position + ivec2(0, 1)) +
              sampleRgb(position + ivec2(1, 1))) * 0.25;
  float u = dot(rgb, vec3(-38.0, -74.0, 112.0)) / 256.0 +
            128.0 / 255.0;
  float v = dot(rgb, vec3(112.0, -94.0, -18.0)) / 256.0 +
            128.0 / 255.0;
  outputColor = vec4(clamp(u, 0.0, 1.0), clamp(v, 0.0, 1.0), 0.0, 1.0);
}
)";

struct NativeHandleDeleter {
  void operator()(native_handle_t *handle) const {
    if (handle != nullptr) {
      native_handle_delete(handle);
    }
  }
};

using NativeHandle = std::unique_ptr<native_handle_t, NativeHandleDeleter>;

bool HasExtension(const char *extensions, const char *name) {
  if (extensions == nullptr || name == nullptr || *name == '\0' ||
      std::strchr(name, ' ') != nullptr) {
    return false;
  }
  const size_t length = std::strlen(name);
  const char *found = extensions;
  while ((found = std::strstr(found, name)) != nullptr) {
    const bool startsWord = found == extensions || found[-1] == ' ';
    const bool endsWord = found[length] == '\0' || found[length] == ' ';
    if (startsWord && endsWord) {
      return true;
    }
    found += length;
  }
  return false;
}

bool IsRgbFormat(uint32_t format) {
  switch (format) {
  case DRM_FORMAT_ABGR8888:
  case DRM_FORMAT_ARGB8888:
  case DRM_FORMAT_XBGR8888:
  case DRM_FORMAT_XRGB8888:
  case DRM_FORMAT_RGB565:
    return true;
  default:
    return false;
  }
}

GLuint CompileShader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  if (shader == 0) {
    return 0;
  }
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }
  char log[1024]{};
  glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
  ALOGE("GLES shader compilation failed: %s", log);
  glDeleteShader(shader);
  return 0;
}

GLuint CreateProgram(const char *fragmentSource) {
  const GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
  if (vertex == 0 || fragment == 0) {
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  }
  const GLuint program = glCreateProgram();
  if (program != 0) {
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
  }
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  if (program != 0) {
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
  }
  if (linked == GL_TRUE) {
    return program;
  }
  char log[1024]{};
  if (program != 0) {
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    glDeleteProgram(program);
  }
  ALOGE("GLES program link failed: %s", log);
  return 0;
}

} // namespace

class GlesFrameConverter::Impl {
public:
  ~Impl() { Reset(); }

  c2_status_t Initialize() {
    if (display_ != EGL_NO_DISPLAY) {
      return C2_OK;
    }
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY ||
        eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
      return FailEgl("initializing EGL");
    }
    const char *extensions = eglQueryString(display_, EGL_EXTENSIONS);
    if (!HasExtension(extensions, "EGL_EXT_image_dma_buf_import") ||
        !HasExtension(extensions, "EGL_KHR_image_base")) {
      ALOGE("EGL DMA-BUF image import is unavailable");
      Reset();
      return C2_OMITTED;
    }
    supports_modifiers_ =
        HasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
    create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    image_target_texture_ = reinterpret_cast<ImageTargetTextureProc>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (create_image_ == nullptr || destroy_image_ == nullptr ||
        image_target_texture_ == nullptr) {
      ALOGE("required EGL image entry points are unavailable");
      Reset();
      return C2_OMITTED;
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
      return FailEgl("binding the GLES API");
    }
    const EGLint configAttributes[] = {EGL_SURFACE_TYPE,
                                       EGL_PBUFFER_BIT,
                                       EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_ES3_BIT_KHR,
                                       EGL_RED_SIZE,
                                       8,
                                       EGL_GREEN_SIZE,
                                       8,
                                       EGL_BLUE_SIZE,
                                       8,
                                       EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (eglChooseConfig(display_, configAttributes, &config, 1, &count) !=
            EGL_TRUE ||
        count != 1) {
      return FailEgl("choosing an EGL config");
    }
    const EGLint surfaceAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, config, surfaceAttributes);
    if (surface_ == EGL_NO_SURFACE) {
      return FailEgl("creating an EGL pbuffer");
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                        EGL_NONE};
    context_ =
        eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttributes);
    if (context_ == EGL_NO_CONTEXT ||
        eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
      return FailEgl("creating a GLES 3 context");
    }

    luma_program_ = CreateProgram(kLumaShader);
    chroma_program_ = CreateProgram(kChromaShader);
    if (luma_program_ == 0 || chroma_program_ == 0) {
      Reset();
      return C2_OMITTED;
    }
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    ALOGI("GLES RGB-to-NV12 DMA-BUF conversion is available");
    return C2_OK;
  }

  void Reset() {
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT &&
        surface_ != EGL_NO_SURFACE) {
      (void)eglMakeCurrent(display_, surface_, surface_, context_);
      glDeleteProgram(chroma_program_);
      glDeleteProgram(luma_program_);
    }
    chroma_program_ = 0;
    luma_program_ = 0;
    if (display_ != EGL_NO_DISPLAY) {
      (void)eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                           EGL_NO_CONTEXT);
      if (context_ != EGL_NO_CONTEXT) {
        (void)eglDestroyContext(display_, context_);
      }
      if (surface_ != EGL_NO_SURFACE) {
        (void)eglDestroySurface(display_, surface_);
      }
      (void)eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    context_ = EGL_NO_CONTEXT;
    surface_ = EGL_NO_SURFACE;
    supports_modifiers_ = false;
    create_image_ = nullptr;
    destroy_image_ = nullptr;
    image_target_texture_ = nullptr;
  }

  c2_status_t Convert(const C2ConstGraphicBlock &source,
                      const MinigbmDmaBuf &destination) {
    if (Initialize() != C2_OK) {
      return C2_OMITTED;
    }
    NativeHandle handle(
        android::UnwrapNativeCodec2GrallocHandle(source.handle()));
    MinigbmDmaBuf input;
    if (handle == nullptr ||
        !GetMinigbmDmaBuf(handle.get(), source.width(), source.height(),
                          &input) ||
        !IsRgbFormat(input.drm_format) || input.plane_count != 1) {
      ALOGE("GLES conversion requires a single-plane RGB minigbm input");
      return C2_OMITTED;
    }
    if (destination.drm_format != DRM_FORMAT_NV12 ||
        destination.modifier != DRM_FORMAT_MOD_LINEAR ||
        destination.plane_count != 2 || source.width() != destination.width ||
        source.height() != destination.height) {
      return C2_BAD_VALUE;
    }
    if (destination.width >
            static_cast<uint32_t>(std::numeric_limits<EGLint>::max()) ||
        destination.height >
            static_cast<uint32_t>(std::numeric_limits<EGLint>::max()) ||
        input.planes[0].stride >
            static_cast<uint32_t>(std::numeric_limits<EGLint>::max()) ||
        destination.planes[0].stride >
            static_cast<uint32_t>(std::numeric_limits<EGLint>::max())) {
      return C2_BAD_VALUE;
    }
    if (input.modifier != DRM_FORMAT_MOD_LINEAR &&
        input.modifier != DRM_FORMAT_MOD_INVALID && !supports_modifiers_) {
      ALOGE("the RGB input requires EGL DMA-BUF modifier import");
      return C2_OMITTED;
    }
    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
      return FailEgl("making the GLES conversion context current");
    }
    while (glGetError() != GL_NO_ERROR) {
    }

    const EGLImageKHR sourceImage = CreateImage(
        input.width, input.height, input.drm_format, input.planes[0].fd,
        input.planes[0].offset, input.planes[0].stride, input.modifier);
    const EGLImageKHR yImage =
        CreateImage(destination.width, destination.height, DRM_FORMAT_R8,
                    destination.planes[0].fd, destination.planes[0].offset,
                    destination.planes[0].stride, DRM_FORMAT_MOD_LINEAR);
    const EGLImageKHR uvImage = CreateImage(
        destination.width / 2, destination.height / 2, DRM_FORMAT_RG88,
        destination.planes[1].fd, destination.planes[1].offset,
        destination.planes[1].stride, DRM_FORMAT_MOD_LINEAR);
    if (sourceImage == EGL_NO_IMAGE_KHR || yImage == EGL_NO_IMAGE_KHR ||
        uvImage == EGL_NO_IMAGE_KHR) {
      DestroyImage(sourceImage);
      DestroyImage(yImage);
      DestroyImage(uvImage);
      return C2_OMITTED;
    }

    GLuint textures[3]{};
    GLuint framebuffer = 0;
    glGenTextures(3, textures);
    glGenFramebuffers(1, &framebuffer);
    const auto cleanup = [&]() {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers(1, &framebuffer);
      glDeleteTextures(3, textures);
      DestroyImage(sourceImage);
      DestroyImage(yImage);
      DestroyImage(uvImage);
    };
    for (size_t index = 0; index < 3; ++index) {
      glBindTexture(GL_TEXTURE_2D, textures[index]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    image_target_texture_(GL_TEXTURE_2D, sourceImage);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    image_target_texture_(GL_TEXTURE_2D, yImage);
    glBindTexture(GL_TEXTURE_2D, textures[2]);
    image_target_texture_(GL_TEXTURE_2D, uvImage);
    const GLenum importError = glGetError();
    if (importError != GL_NO_ERROR) {
      ALOGE("binding DMA-BUF EGL images failed with error %#x", importError);
      cleanup();
      return C2_OMITTED;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    if (!RenderPlane(luma_program_, textures[1], destination.width,
                     destination.height, input.width, input.height) ||
        !RenderPlane(chroma_program_, textures[2], destination.width / 2,
                     destination.height / 2, input.width, input.height)) {
      cleanup();
      return C2_CORRUPTED;
    }
    // V4L2 has no explicit input fence in this API. Complete GPU writes before
    // queueing the same DMA-BUF to Venus.
    glFinish();
    const GLenum error = glGetError();
    cleanup();
    if (error != GL_NO_ERROR) {
      ALOGE("GLES RGB-to-NV12 conversion failed with error %#x", error);
      return C2_CORRUPTED;
    }
    return C2_OK;
  }

private:
  c2_status_t FailEgl(const char *operation) {
    ALOGE("%s failed with EGL error %#x", operation, eglGetError());
    Reset();
    return C2_OMITTED;
  }

  EGLImageKHR CreateImage(uint32_t width, uint32_t height, uint32_t format,
                          int fd, uint32_t offset, uint32_t stride,
                          uint64_t modifier) const {
    std::vector<EGLint> attributes = {
        EGL_WIDTH,
        static_cast<EGLint>(width),
        EGL_HEIGHT,
        static_cast<EGLint>(height),
        EGL_LINUX_DRM_FOURCC_EXT,
        static_cast<EGLint>(format),
        EGL_DMA_BUF_PLANE0_FD_EXT,
        fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        static_cast<EGLint>(offset),
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<EGLint>(stride),
    };
    if (supports_modifiers_ && modifier != DRM_FORMAT_MOD_INVALID) {
      attributes.insert(attributes.end(),
                        {
                            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                            static_cast<EGLint>(modifier & 0xffffffffu),
                            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                            static_cast<EGLint>(modifier >> 32),
                        });
    }
    attributes.push_back(EGL_NONE);
    EGLImageKHR image =
        create_image_(display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr,
                      attributes.data());
    if (image == EGL_NO_IMAGE_KHR) {
      ALOGE("importing DRM format %#x as an EGL image failed with error %#x",
            format, eglGetError());
    }
    return image;
  }

  void DestroyImage(EGLImageKHR image) const {
    if (image != EGL_NO_IMAGE_KHR) {
      (void)destroy_image_(display_, image);
    }
  }

  bool RenderPlane(GLuint program, GLuint destination, uint32_t width,
                   uint32_t height, uint32_t sourceWidth,
                   uint32_t sourceHeight) const {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           destination, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      ALOGE("GLES NV12 plane framebuffer is incomplete");
      return false;
    }
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "sourceTexture"), 0);
    glUniform2i(glGetUniformLocation(program, "sourceSize"),
                static_cast<GLint>(sourceWidth),
                static_cast<GLint>(sourceHeight));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
  }

  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLSurface surface_ = EGL_NO_SURFACE;
  bool supports_modifiers_ = false;
  PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
  ImageTargetTextureProc image_target_texture_ = nullptr;
  GLuint luma_program_ = 0;
  GLuint chroma_program_ = 0;
};

GlesFrameConverter::GlesFrameConverter() : impl_(std::make_unique<Impl>()) {}

GlesFrameConverter::~GlesFrameConverter() = default;

c2_status_t GlesFrameConverter::Initialize() { return impl_->Initialize(); }

c2_status_t GlesFrameConverter::Convert(const C2ConstGraphicBlock &source,
                                        const MinigbmDmaBuf &destination) {
  return impl_->Convert(source, destination);
}

void GlesFrameConverter::Reset() { impl_->Reset(); }

} // namespace floral::codec
