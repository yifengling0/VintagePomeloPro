/* Native GLES sRGB alias experiment. Diagnostic only: invoked on an isolated
 * context before VirGL initialization. Tiny readbacks verify GPU results;
 * no production caps or resources are changed. Not a conformance suite. */
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <native_buffer/native_buffer.h>
#include <native_buffer/buffer_common.h>
#include <native_window/external_window.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* OHOS SDK EGL extension; libepoxy does not carry this platform token. */
#ifndef EGL_NATIVE_BUFFER_OHOS
#define EGL_NATIVE_BUFFER_OHOS 0x34E1
#endif

struct wh_alias_probe {
   FILE *out;
   unsigned checks, failures;
   GLuint program, sampler_program, vao, fbo, scratch;
   int sync_mode;
   unsigned sync_errors, gpu_waits, gpu_copies;
   GLuint authoritative_alias;
   unsigned alias_levels;
   GLuint alias_tex[2];
   EGLImageKHR alias_image[2];
};

static void wh_alias_check(struct wh_alias_probe *p, const char *route,
                           const char *name, int pass, int actual, int expected)
{
   p->checks++;
   if (!pass) p->failures++;
   fprintf(p->out, "check\t%s\t%s\t%s\tactual=%d\texpected=%d\n",
           route, name, pass ? "PASS" : "FAIL", actual, expected);
   fflush(p->out);
}

static int wh_alias_ext(const char *name)
{
   GLint n = 0;
   glGetIntegerv(GL_NUM_EXTENSIONS, &n);
   for (GLint i = 0; i < n; i++)
      if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, i), name)) return 1;
   return 0;
}

static GLuint wh_alias_program(struct wh_alias_probe *p, const char *frag)
{
   const char *vert = "#version 300 es\n"
      "void main(){vec2 v=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
      "gl_Position=vec4(v*2.0-1.0,0,1);}";
   GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
   GLuint prog = glCreateProgram();
   glShaderSource(vs, 1, &vert, NULL); glCompileShader(vs);
   glShaderSource(fs, 1, &frag, NULL); glCompileShader(fs);
   glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
   GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[2048]; glGetProgramInfoLog(prog, sizeof(log), NULL, log);
      fprintf(p->out, "shader_failure\t%s\n", log);
      glDeleteProgram(prog); prog = 0;
   }
   glDeleteShader(vs); glDeleteShader(fs);
   return prog;
}

/* Separate imported siblings are not automatically tracked as one GL object.
 * Probe GPU barriers, server-side EGL fence waits, CPU completion, and rebind. */
static void wh_alias_sync(struct wh_alias_probe *p)
{
   switch (p->sync_mode) {
   case 1: glFlush(); break;
   case 2: glMemoryBarrier(GL_ALL_BARRIER_BITS); break;
   case 3: glFinish(); break;
   case 4: case 5: {
      PFNEGLCREATESYNCKHRPROC create=(PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
      PFNEGLWAITSYNCKHRPROC wait=(PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
      PFNEGLDESTROYSYNCKHRPROC destroy=(PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
      if (create && wait && destroy) {
         EGLDisplay d=eglGetCurrentDisplay();
         EGLSyncKHR fence=create(d,EGL_SYNC_FENCE_KHR,NULL);
         if(fence != EGL_NO_SYNC_KHR) {
            glFlush();
            if(wait(d,fence,0) != EGL_TRUE) p->sync_errors++;
            else p->gpu_waits++;
            if(!destroy(d,fence)) p->sync_errors++;
         } else p->sync_errors++;
      }
      else p->sync_errors++;
      if (p->sync_mode == 5) {
         typedef void (*target_fn)(GLenum,GLeglImageOES);
         target_fn target=(target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
         GLint binding=0; glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
         if(target) for(int i=0;i<2;i++) if(p->alias_tex[i] && p->alias_image[i] != EGL_NO_IMAGE_KHR) {
            glBindTexture(GL_TEXTURE_2D,p->alias_tex[i]); target(GL_TEXTURE_2D,p->alias_image[i]);
         }
         glBindTexture(GL_TEXTURE_2D,binding);
      }
      break;
   }
   default: break;
   }
}

static GLenum wh_alias_attach(struct wh_alias_probe *p, GLuint texture, int level)
{
   wh_alias_sync(p);
   if (p->sync_mode == 6 && p->authoritative_alias) {
      GLuint src=p->authoritative_alias;
      GLuint dst=src == p->alias_tex[0] ? p->alias_tex[1] : p->alias_tex[0];
      if (texture != src) {
         for(unsigned l=0;l<p->alias_levels;l++) {
            int size=4>>l;
            glCopyImageSubData(src,GL_TEXTURE_2D,l,0,0,0,dst,GL_TEXTURE_2D,l,0,0,0,size,size,1);
            p->gpu_copies++;
         }
      }
   }
   if (p->sync_mode == 6 && (texture == p->alias_tex[0] || texture == p->alias_tex[1]))
      p->authoritative_alias=texture;
   glBindFramebuffer(GL_FRAMEBUFFER, p->fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);
   return glCheckFramebufferStatus(GL_FRAMEBUFFER);
}

static void wh_alias_pixel(struct wh_alias_probe *p, const char *route, const char *name,
                           GLuint texture, int level, int red, int alpha)
{
   unsigned char pixel[4] = {0};
   wh_alias_attach(p, texture, level);
   glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
   GLenum error = glGetError();
   wh_alias_check(p, route, name,
      error == GL_NO_ERROR && abs((int)pixel[0]-red) <= 2 &&
      abs((int)pixel[1]-red) <= 2 && abs((int)pixel[2]-red) <= 2 &&
      abs((int)pixel[3]-alpha) <= 2, pixel[0], red);
   if (error) fprintf(p->out, "pixel_error\t%s\t%s\t0x%x\n", route, name, error);
}

static void wh_alias_draw(struct wh_alias_probe *p, float gray, float alpha)
{
   glUseProgram(p->program);
   glUniform4f(glGetUniformLocation(p->program, "color"), gray, gray, gray, alpha);
   glDrawArrays(GL_TRIANGLES, 0, 3);
}

static float wh_alias_decode(int byte)
{
   float x=byte/255.0f;
   return x <= .04045f ? x/12.92f : powf((x+.055f)/1.055f,2.4f);
}

static int wh_alias_encode(float x)
{
   float s=x <= .0031308f ? 12.92f*x : 1.055f*powf(x,1.0f/2.4f)-.055f;
   return (int)floorf(s*255.0f+.5f);
}

static void wh_alias_suite(struct wh_alias_probe *p, const char *route,
                           GLuint linear, GLuint srgb, int mip)
{
   for (int i=0;i<32 && glGetError();i++) {}
   GLenum complete = wh_alias_attach(p, linear, 0);
   wh_alias_check(p, route, "linear_fbo", complete == GL_FRAMEBUFFER_COMPLETE, complete, GL_FRAMEBUFFER_COMPLETE);
   if (complete != GL_FRAMEBUFFER_COMPLETE) return;
   GLint encoding = 0;
   glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                         GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
   wh_alias_check(p, route, "linear_encoding", encoding == GL_LINEAR, encoding, GL_LINEAR);
   complete = wh_alias_attach(p, srgb, 0);
   wh_alias_check(p, route, "srgb_fbo", complete == GL_FRAMEBUFFER_COMPLETE, complete, GL_FRAMEBUFFER_COMPLETE);
   if (complete != GL_FRAMEBUFFER_COMPLETE) return;
   glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                         GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
   wh_alias_check(p, route, "srgb_encoding", encoding == GL_SRGB, encoding, GL_SRGB);
   glViewport(0, 0, 4, 4);
   glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST);
   glDisable(GL_DITHER); glBindVertexArray(p->vao);
   /* Linear destination: .25 -> 64 raw bytes, both aliases see the same backing. */
   wh_alias_attach(p, linear, 0); glClearColor(.25f,.25f,.25f,1); glClear(GL_COLOR_BUFFER_BIT);
   wh_alias_pixel(p, route, "linear_clear_shared_bytes", srgb, 0, 64, 255);
   /* GLES automatically encodes when the actual attachment is sRGB. */
   wh_alias_attach(p, srgb, 0); wh_alias_draw(p, .25f, 1);
   wh_alias_pixel(p, route, "srgb_draw_shared_bytes", linear, 0, 137, 255);
   wh_alias_attach(p, linear, 0); wh_alias_draw(p, .25f, 1);
   wh_alias_pixel(p, route, "linear_draw_shared_bytes", srgb, 0, 64, 255);
   wh_alias_attach(p, srgb, 0); glClearColor(.25f,.25f,.25f,1); glClear(GL_COLOR_BUFFER_BIT);
   wh_alias_pixel(p, route, "srgb_clear_shared_bytes", linear, 0, 137, 255);
   wh_alias_attach(p, srgb, 0);
   glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD);
   glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
   wh_alias_draw(p, .5f, .5f); glDisable(GL_BLEND);
   wh_alias_pixel(p, route, "linearized_destination_blend", linear, 0, 165, 128);
   /* A raw 128 byte sampled through an sRGB view decodes to .216 (55). */
   wh_alias_attach(p, linear, 0); glClearColor(.5f,.5f,.5f,1); glClear(GL_COLOR_BUFFER_BIT);
   wh_alias_attach(p, p->scratch, 0);
   glUseProgram(p->sampler_program); glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, srgb);
   glUniform1i(glGetUniformLocation(p->sampler_program, "tex"), 0);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   wh_alias_pixel(p, route, "srgb_sample_decode", p->scratch, 0, 55, 255);
   glBindTexture(GL_TEXTURE_2D, 0);
   /* Repeated switching, including two GPU operations before any readback. */
   for (int i = 0; i < 3; i++) {
      wh_alias_attach(p, srgb, 0); wh_alias_draw(p, .25f, 1);
      wh_alias_attach(p, linear, 0); wh_alias_draw(p, .5f, 1);
   }
   wh_alias_pixel(p, route, "alternating_views", srgb, 0, 128, 255);
   if (mip) {
      wh_alias_attach(p, srgb, 1); glViewport(0, 0, 2, 2); wh_alias_draw(p, .25f, 1);
      wh_alias_pixel(p, route, "mip1_shared_bytes", linear, 1, 137, 255);
      wh_alias_pixel(p, route, "mip0_preserved", linear, 0, 128, 255);
   }
   /* No readback / glFinish in this dependency chain. Each blended write
    * depends on the byte content written through the other view. */
   glViewport(0,0,4,4);
   wh_alias_attach(p,linear,0); glClearColor(.25f,.25f,.25f,1); glClear(GL_COLOR_BUFFER_BIT);
   int reference=64;
   glEnable(GL_BLEND);
   glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ZERO);
   for(int i=0;i<16;i++) {
      wh_alias_attach(p,srgb,0); wh_alias_draw(p,.5f,.5f);
      reference=wh_alias_encode((.5f+wh_alias_decode(reference))*.5f);
      wh_alias_attach(p,linear,0); wh_alias_draw(p,.25f,.5f);
      reference=(int)floorf(((.25f+reference/255.0f)*.5f)*255.0f+.5f);
   }
   glDisable(GL_BLEND);
   wh_alias_pixel(p,route,"gpu_only_32_dependent_writes",srgb,0,reference,128);
   /* Partial color masks preserve the untouched channels in the same backing. */
   wh_alias_attach(p,linear,0); glClearColor(.5f,.5f,.5f,1); glClear(GL_COLOR_BUFFER_BIT);
   wh_alias_attach(p,srgb,0); glColorMask(GL_TRUE,GL_FALSE,GL_FALSE,GL_FALSE);
   wh_alias_draw(p,.25f,1); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
   unsigned char masked[4]={0};
   wh_alias_attach(p,linear,0); glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,masked);
   wh_alias_check(p,route,"color_mask_preserve_shared_channels",
      abs((int)masked[0]-137)<=2 && abs((int)masked[1]-128)<=2 &&
      abs((int)masked[2]-128)<=2 && masked[3]==255,masked[0],137);
   wh_alias_check(p,route,"synchronization_calls",p->sync_errors==0,p->sync_errors,0);
   fprintf(p->out,"synchronization\t%s\tgpu_waits=%u\terrors=%u\n",route,p->gpu_waits,p->sync_errors);
   GLenum error = glGetError();
   wh_alias_check(p, route, "suite_gl_error", error == GL_NO_ERROR, error, GL_NO_ERROR);
}

typedef void (*wh_texture_view_fn)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
typedef void (*wh_image_target_fn)(GLenum, GLeglImageOES);

static void wh_alias_texture_views(struct wh_alias_probe *p)
{
   const char *name = wh_alias_ext("GL_OES_texture_view") ? "glTextureViewOES" :
                      wh_alias_ext("GL_EXT_texture_view") ? "glTextureViewEXT" : NULL;
   if (!name) { fprintf(p->out, "route\ttexture_view\tUNAVAILABLE\tno_advertised_extension\n"); return; }
   wh_texture_view_fn view = (wh_texture_view_fn)eglGetProcAddress(name);
   if (!view) { fprintf(p->out, "route\ttexture_view\tUNAVAILABLE\tno_entrypoint\n"); return; }
   GLuint base = 0, tex[2] = {0};
   glGenTextures(1, &base); glBindTexture(GL_TEXTURE_2D, base);
   glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 4, 4);
   glGenTextures(2, tex);
   view(tex[0], GL_TEXTURE_2D, base, GL_RGBA8, 0, 2, 0, 1);
   view(tex[1], GL_TEXTURE_2D, base, GL_SRGB8_ALPHA8, 0, 2, 0, 1);
   GLenum error = glGetError();
   fprintf(p->out, "route\ttexture_view\tcreate_error=0x%x\n", error);
   if (!error) {
      for (int i=0;i<2;i++) {
         glBindTexture(GL_TEXTURE_2D, tex[i]);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      }
      wh_alias_suite(p, "texture_view", tex[0], tex[1], 1);
   }
   glDeleteTextures(2, tex); glDeleteTextures(1, &base);
}

static void wh_alias_images(struct wh_alias_probe *p, const char *route,
                            EGLenum target, EGLClientBuffer buffer, EGLContext context, int mode)
{
   PFNEGLCREATEIMAGEKHRPROC create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   PFNEGLDESTROYIMAGEKHRPROC destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
   wh_image_target_fn image_target = (wh_image_target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
   if (!create || !destroy || !image_target) {
      fprintf(p->out,"route\t%s\tUNAVAILABLE\tmissing_entrypoint\n",route); return;
   }
   EGLDisplay display = eglGetCurrentDisplay();
   EGLImageKHR images[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
   GLuint tex[2] = {0};
   for (int i=0;i<2;i++) {
      EGLint attrs[] = {EGL_GL_COLORSPACE_KHR,
         i ? EGL_GL_COLORSPACE_SRGB_KHR : EGL_GL_COLORSPACE_LINEAR_KHR,
         EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
      images[i] = create(display,context,target,buffer,attrs);
      EGLint error = eglGetError();
      fprintf(p->out,"image\t%s\t%s\tcreated=%d\tegl_error=0x%x\n",route,
              i ? "srgb" : "linear",images[i] != EGL_NO_IMAGE_KHR,error);
      if (images[i] == EGL_NO_IMAGE_KHR) goto done;
      glGenTextures(1,&tex[i]); glBindTexture(GL_TEXTURE_2D,tex[i]);
      image_target(GL_TEXTURE_2D,images[i]);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      error = glGetError();
      fprintf(p->out,"image_target\t%s\t%s\tgl_error=0x%x\n",route,i ? "srgb" : "linear",error);
      if (error) goto done;
   }
   const char *modes[]={"none","flush","memory_barrier","finish","egl_server_wait","egl_wait_rebind"};
   char label[128]; snprintf(label,sizeof(label),"%s_%s",route,modes[mode]);
   p->sync_mode=mode; p->sync_errors=p->gpu_waits=0;
   memcpy(p->alias_tex,tex,sizeof(tex)); memcpy(p->alias_image,images,sizeof(images));
   wh_alias_suite(p,label,tex[0],tex[1],0);
   p->sync_mode=0; memset(p->alias_tex,0,sizeof(p->alias_tex)); memset(p->alias_image,0,sizeof(p->alias_image));
done:
   glBindFramebuffer(GL_FRAMEBUFFER,0);
   glBindTexture(GL_TEXTURE_2D,0);
   glDeleteTextures(2,tex);
   for (int i=0;i<2;i++) if (images[i] != EGL_NO_IMAGE_KHR) destroy(display,images[i]);
}

/* This is not aliasing: two ordinary GPU textures plus raw image copies.
 * It qualifies a possible GPU-only fallback for resources NativeBuffer cannot
 * represent, including mip chains. Production must track dirty subresources. */
static void wh_alias_gpu_copy(struct wh_alias_probe *p)
{
   GLuint tex[2]={0}; glGenTextures(2,tex);
   for(int i=0;i<2;i++) {
      glBindTexture(GL_TEXTURE_2D,tex[i]);
      glTexStorage2D(GL_TEXTURE_2D,2,i ? GL_SRGB8_ALPHA8 : GL_RGBA8,4,4);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
   }
   p->sync_mode=6; p->sync_errors=p->gpu_waits=p->gpu_copies=0;
   p->authoritative_alias=0; p->alias_levels=2;
   memcpy(p->alias_tex,tex,sizeof(tex));
   wh_alias_suite(p,"ordinary_texture_raw_gpu_copy",tex[0],tex[1],1);
   fprintf(p->out,"copy_statistics\tgpu_copies=%u\tcpu_texture_copies=0\n",p->gpu_copies);
   p->sync_mode=0; p->authoritative_alias=0; p->alias_levels=0;
   memset(p->alias_tex,0,sizeof(p->alias_tex));
   glBindFramebuffer(GL_FRAMEBUFFER,0); glBindTexture(GL_TEXTURE_2D,0);
   glDeleteTextures(2,tex);
}

/* Rule out source format and image creation order as causes of the
 * texture-backed rejection. Create both images before importing either. */
static void wh_alias_ordinary_image_orders(struct wh_alias_probe *p)
{
   PFNEGLCREATEIMAGEKHRPROC create=(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   PFNEGLDESTROYIMAGEKHRPROC destroy=(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
   wh_image_target_fn target=(wh_image_target_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
   if(!create || !destroy || !target) return;
   EGLDisplay d=eglGetCurrentDisplay();
   for(int base_srgb=0;base_srgb<2;base_srgb++) for(int reverse=0;reverse<2;reverse++) {
      char label[128]; snprintf(label,sizeof(label),"texture_image_precreate_%s_%s_first",
         base_srgb ? "srgb8" : "rgba8",reverse ? "srgb" : "linear");
      GLuint base=0,tex[2]={0};
      EGLImageKHR images[2]={EGL_NO_IMAGE_KHR,EGL_NO_IMAGE_KHR};
      glGenTextures(1,&base); glBindTexture(GL_TEXTURE_2D,base);
      glTexStorage2D(GL_TEXTURE_2D,1,base_srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,4,4);
      for(int j=0;j<2;j++) {
         int i=reverse ? 1-j : j;
         const EGLint attrs[]={EGL_GL_COLORSPACE_KHR,
            i ? EGL_GL_COLORSPACE_SRGB_KHR : EGL_GL_COLORSPACE_LINEAR_KHR,
            EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
         images[i]=create(d,eglGetCurrentContext(),EGL_GL_TEXTURE_2D_KHR,
                          (EGLClientBuffer)(uintptr_t)base,attrs);
         EGLint error=eglGetError();
         fprintf(p->out,"image_order\t%s\t%s\tcreated=%d\tegl_error=0x%x\n",
            label,i ? "srgb" : "linear",images[i]!=EGL_NO_IMAGE_KHR,error);
         if(images[i]==EGL_NO_IMAGE_KHR) break;
      }
      if(images[0]!=EGL_NO_IMAGE_KHR && images[1]!=EGL_NO_IMAGE_KHR) {
         glGenTextures(2,tex);
         for(int i=0;i<2;i++) {
            glBindTexture(GL_TEXTURE_2D,tex[i]); target(GL_TEXTURE_2D,images[i]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
         }
         p->sync_mode=4; p->sync_errors=p->gpu_waits=0;
         memcpy(p->alias_tex,tex,sizeof(tex)); memcpy(p->alias_image,images,sizeof(images));
         wh_alias_suite(p,label,tex[0],tex[1],0);
         p->sync_mode=0;
         memset(p->alias_tex,0,sizeof(p->alias_tex)); memset(p->alias_image,0,sizeof(p->alias_image));
      }
      glBindFramebuffer(GL_FRAMEBUFFER,0); glBindTexture(GL_TEXTURE_2D,0);
      glDeleteTextures(2,tex);
      for(int i=0;i<2;i++) if(images[i]!=EGL_NO_IMAGE_KHR) destroy(d,images[i]);
      glDeleteTextures(1,&base);
   }
}

static void wh_alias_native_buffer(struct wh_alias_probe *p, int mode)
{
   void *blib=dlopen("libnative_buffer.so",RTLD_NOW|RTLD_LOCAL);
   void *wlib=dlopen("libnative_window.so",RTLD_NOW|RTLD_LOCAL);
   OH_NativeBuffer *(*alloc)(const OH_NativeBuffer_Config*) = NULL;
   int32_t (*unref)(OH_NativeBuffer*) = NULL;
   OHNativeWindowBuffer *(*window_buffer)(OH_NativeBuffer*) = NULL;
   void (*window_destroy)(OHNativeWindowBuffer*) = NULL;
   void *symbol;
#define WH_LOAD(lib,fn,name) do {symbol=lib ? dlsym(lib,name) : NULL; memcpy(&fn,&symbol,sizeof(fn));} while (0)
   WH_LOAD(blib,alloc,"OH_NativeBuffer_Alloc");
   WH_LOAD(blib,unref,"OH_NativeBuffer_Unreference");
   WH_LOAD(wlib,window_buffer,"OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer");
   WH_LOAD(wlib,window_destroy,"OH_NativeWindow_DestroyNativeWindowBuffer");
#undef WH_LOAD
   if (alloc && unref && window_buffer && window_destroy) {
      OH_NativeBuffer_Config cfg = {0};
      cfg.width=cfg.height=4; cfg.format=NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
      cfg.usage=NATIVEBUFFER_USAGE_HW_RENDER|NATIVEBUFFER_USAGE_HW_TEXTURE|NATIVEBUFFER_USAGE_MEM_DMA;
      OH_NativeBuffer *native = alloc(&cfg);
      fprintf(p->out,"native_buffer\tallocated=%d\n",native != NULL);
      if (native) {
         OHNativeWindowBuffer *window = window_buffer(native);
         if (window) {
            wh_alias_images(p,"native_buffer_egl_image",EGL_NATIVE_BUFFER_OHOS,
                            (EGLClientBuffer)window,EGL_NO_CONTEXT,mode);
            window_destroy(window);
         }
         unref(native);
      }
   } else fprintf(p->out,"route\tnative_buffer_egl_image\tUNAVAILABLE\tmissing_native_api\n");
   if (wlib) dlclose(wlib);
   if (blib) dlclose(blib);
}

static void winehua_srgb_alias_probe(void)
{
   struct wh_alias_probe p = {0};
   p.out=fopen("/data/storage/el2/base/temp/gl-srgb-alias-probe.tsv","w");
   if (!p.out) return;
   fprintf(p.out,"schema\t1\tdiagnostic_only=1\tproduction_caps_unchanged=1\n");
   fprintf(p.out,"host\tGL_VERSION\t%s\n",glGetString(GL_VERSION));
   fprintf(p.out,"host\tGL_RENDERER\t%s\n",glGetString(GL_RENDERER));
   const char *names[]={"GL_OES_texture_view","GL_EXT_texture_view",
      "GL_EXT_sRGB_write_control","GL_EXT_texture_sRGB_decode","GL_OES_EGL_image",
      "GL_EXT_sRGB","GL_EXT_color_buffer_float"};
   for (unsigned i=0;i<sizeof(names)/sizeof(*names);i++)
      fprintf(p.out,"extension\t%s\t%d\n",names[i],wh_alias_ext(names[i]));
   fprintf(p.out,"egl_extensions\t%s\n",eglQueryString(eglGetCurrentDisplay(),EGL_EXTENSIONS));
   fflush(p.out);
   p.program=wh_alias_program(&p,"#version 300 es\nprecision highp float;"
      "uniform vec4 color;out vec4 result;void main(){result=color;}");
   p.sampler_program=wh_alias_program(&p,"#version 300 es\nprecision highp float;"
      "uniform sampler2D tex;out vec4 result;void main(){result=texture(tex,vec2(.5));}");
   if (!p.program || !p.sampler_program) goto done;
   glGenVertexArrays(1,&p.vao); glGenFramebuffers(1,&p.fbo);
   glGenTextures(1,&p.scratch); glBindTexture(GL_TEXTURE_2D,p.scratch);
   glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,4,4);
   wh_alias_texture_views(&p);
   /* First prove identical physical storage with GL textures, then NativeBuffer. */
   GLuint base = 0;
   glGenTextures(1,&base); glBindTexture(GL_TEXTURE_2D,base);
   glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,4,4);
   wh_alias_images(&p,"texture_egl_image",EGL_GL_TEXTURE_2D_KHR,
                   (EGLClientBuffer)(uintptr_t)base,eglGetCurrentContext(),0);
   glDeleteTextures(1,&base);
   /* Fresh backing and images per policy; GPU server wait runs before Finish.
    * Repeated cold creation prevents inheriting completion from a prior route. */
   wh_alias_ordinary_image_orders(&p);
   wh_alias_gpu_copy(&p);
   const int order[]={4,0,1,2,5,3};
   for(unsigned i=0;i<sizeof(order)/sizeof(*order);i++) wh_alias_native_buffer(&p,order[i]);
done:
   glBindFramebuffer(GL_FRAMEBUFFER,0); glBindVertexArray(0); glUseProgram(0);
   glBindTexture(GL_TEXTURE_2D,0);
   glDeleteTextures(1,&p.scratch); glDeleteFramebuffers(1,&p.fbo);
   glDeleteVertexArrays(1,&p.vao);
   if(p.program) glDeleteProgram(p.program);
   if(p.sampler_program) glDeleteProgram(p.sampler_program);
   fprintf(p.out,"summary\tchecks=%u\tfailures=%u\n",p.checks,p.failures);
   fclose(p.out);
   for (int i=0;i<32 && glGetError();i++) {}
}
