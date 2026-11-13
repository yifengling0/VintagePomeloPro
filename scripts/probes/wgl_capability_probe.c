/* Real WGL profile/version and shader probes; no version overrides. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>

typedef HGLRC (WINAPI *create_attribs_fn)(HDC, HGLRC, const int *);
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_FLAGS_ARB 0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 1
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 2

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Exercise actual sampling/conversion rather than trusting extension strings.
 * These checks run only in the compatibility context. */
static void texture_samples(FILE *out)
{
    static const unsigned char rgba[] = {51,102,153,255};
    static const unsigned char bgra[] = {153,102,51,255};
    static const unsigned char alpha[] = {128};
    static const unsigned char rgba_half_alpha[] = {255,0,0,128};
    static const unsigned short rgb565[] = {0xf800};
    static const unsigned short rgba4444[] = {0xf00f};
    static const unsigned char dxt1[] = {0x00,0xf8,0,0,0,0,0,0};
    static const unsigned char dxt3[] = {0x88,0x88,0x88,0x88,0x88,0x88,0x88,0x88,0x00,0xf8,0,0,0,0,0,0};
    static const unsigned char dxt5[] = {128,0,0,0,0,0,0,0,0x00,0xf8,0,0,0,0,0,0};
    static const struct {
        const char *name;
        GLint internal;
        GLenum format, type;
        const void *data;
        unsigned char expected[4];
        int compressed, blend;
    } tests[] = {
        {"RGBA8",GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,rgba,{51,102,153,255},0,0},
        {"BGRA8",GL_RGBA8,GL_BGRA,GL_UNSIGNED_BYTE,bgra,{51,102,153,255},0,0},
        {"BGRA8_REV",GL_RGBA8,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,bgra,{51,102,153,255},0,0},
        {"RGB565",GL_RGB,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,rgb565,{255,0,0,255},0,0},
        {"RGBA4444",GL_RGBA4,GL_RGBA,GL_UNSIGNED_SHORT_4_4_4_4,rgba4444,{255,0,0,255},0,0},
        {"DXT1",GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,0,0,dxt1,{255,0,0,255},1,0},
        {"SRGB8_ALPHA8_decode",GL_SRGB8_ALPHA8,GL_RGBA,GL_UNSIGNED_BYTE,rgba,{8,34,81,255},0,0},
        {"ALPHA8_blend_black",GL_ALPHA8,GL_ALPHA,GL_UNSIGNED_BYTE,alpha,{128,128,128,191},0,1},
        {"RGBA8_blend_black",GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,rgba_half_alpha,{128,0,0,191},0,1},
        {"DXT3_blend_black",GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,0,0,dxt3,{136,0,0,191},16,1},
        {"DXT5_blend_black",GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,0,0,dxt5,{128,0,0,191},16,1},
    };
    unsigned i;
    PFNGLCOMPRESSEDTEXIMAGE2DPROC compressed_image = (void *)wglGetProcAddress("glCompressedTexImage2D");
    glViewport(0,0,256,256);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_DITHER); glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    for (i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i) {
        GLuint texture;
        unsigned char pixel[4] = {0};
        unsigned error;
        int channel, ok = 1;
        while (glGetError() != GL_NO_ERROR) {}
        glDisable(GL_BLEND);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        if (tests[i].compressed) {
            if (compressed_image) compressed_image(GL_TEXTURE_2D,0,tests[i].internal,4,4,0,
                tests[i].compressed == 1 ? 8 : tests[i].compressed,tests[i].data);
            else ok = 0;
        } else glTexImage2D(GL_TEXTURE_2D,0,tests[i].internal,1,1,0,tests[i].format,tests[i].type,tests[i].data);
        glEnable(GL_TEXTURE_2D);
        if (tests[i].blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA); }
        glBegin(GL_QUADS);
        glTexCoord2f(0,0); glVertex2f(-1,-1);
        glTexCoord2f(1,0); glVertex2f(1,-1);
        glTexCoord2f(1,1); glVertex2f(1,1);
        glTexCoord2f(0,1); glVertex2f(-1,1);
        glEnd();
        glReadPixels(128,128,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        error = glGetError();
        for (channel = 0; channel < 3; ++channel)
            if (abs((int)pixel[channel]-(int)tests[i].expected[channel]) > 2) ok = 0;
        /* A Windows pixel format may omit alpha storage, so audit RGB here. */
        fprintf(out,"sample\t%s\t%u,%u,%u,%u\t%u,%u,%u,%u\t%d\t%u\n",tests[i].name,
            pixel[0],pixel[1],pixel[2],pixel[3],tests[i].expected[0],tests[i].expected[1],
            tests[i].expected[2],tests[i].expected[3],ok && error == 0,error);
        glBindTexture(GL_TEXTURE_2D,0); glDeleteTextures(1,&texture);
    }
    glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
}

/* Match WineD3D's GLSL 1.20 color/texture varying and separate-alpha blend
 * path. Fixed-function texture tests alone do not exercise this conversion. */
static void legacy_shader_samples(FILE *out)
{
    PFNGLCREATESHADERPROC create_shader = (void *)wglGetProcAddress("glCreateShader");
    PFNGLSHADERSOURCEPROC shader_source = (void *)wglGetProcAddress("glShaderSource");
    PFNGLCOMPILESHADERPROC compile_shader = (void *)wglGetProcAddress("glCompileShader");
    PFNGLGETSHADERIVPROC shader_iv = (void *)wglGetProcAddress("glGetShaderiv");
    PFNGLGETSHADERINFOLOGPROC shader_log = (void *)wglGetProcAddress("glGetShaderInfoLog");
    PFNGLCREATEPROGRAMPROC create_program = (void *)wglGetProcAddress("glCreateProgram");
    PFNGLATTACHSHADERPROC attach_shader = (void *)wglGetProcAddress("glAttachShader");
    PFNGLLINKPROGRAMPROC link_program = (void *)wglGetProcAddress("glLinkProgram");
    PFNGLGETPROGRAMIVPROC program_iv = (void *)wglGetProcAddress("glGetProgramiv");
    PFNGLUSEPROGRAMPROC use_program = (void *)wglGetProcAddress("glUseProgram");
    PFNGLGETUNIFORMLOCATIONPROC uniform_location = (void *)wglGetProcAddress("glGetUniformLocation");
    PFNGLUNIFORM1IPROC uniform1i = (void *)wglGetProcAddress("glUniform1i");
    PFNGLDELETEPROGRAMPROC delete_program = (void *)wglGetProcAddress("glDeleteProgram");
    PFNGLDELETESHADERPROC delete_shader = (void *)wglGetProcAddress("glDeleteShader");
    PFNGLBLENDFUNCSEPARATEPROC blend_func_separate = (void *)wglGetProcAddress("glBlendFuncSeparate");
    PFNGLBLENDEQUATIONSEPARATEPROC blend_equation_separate = (void *)wglGetProcAddress("glBlendEquationSeparate");
    PFNGLBINDATTRIBLOCATIONPROC bind_attrib = (void *)wglGetProcAddress("glBindAttribLocation");
    PFNGLGENBUFFERSPROC gen_buffers = (void *)wglGetProcAddress("glGenBuffers");
    PFNGLBINDBUFFERPROC bind_buffer = (void *)wglGetProcAddress("glBindBuffer");
    PFNGLBUFFERDATAPROC buffer_data = (void *)wglGetProcAddress("glBufferData");
    PFNGLMAPBUFFERPROC map_buffer = (void *)wglGetProcAddress("glMapBuffer");
    PFNGLUNMAPBUFFERPROC unmap_buffer = (void *)wglGetProcAddress("glUnmapBuffer");
    PFNGLDELETEBUFFERSPROC delete_buffers = (void *)wglGetProcAddress("glDeleteBuffers");
    PFNGLVERTEXATTRIBPOINTERPROC attrib_pointer = (void *)wglGetProcAddress("glVertexAttribPointer");
    PFNGLENABLEVERTEXATTRIBARRAYPROC enable_attrib = (void *)wglGetProcAddress("glEnableVertexAttribArray");
    PFNGLDISABLEVERTEXATTRIBARRAYPROC disable_attrib = (void *)wglGetProcAddress("glDisableVertexAttribArray");
    const char *vertex = "#version 120\nvoid main(){gl_Position=gl_Vertex;"
        "gl_FrontColor=gl_Color;gl_BackColor=gl_Color;gl_TexCoord[0]=gl_MultiTexCoord0;}";
    const char *fragment = "#version 120\nuniform sampler2D tex;void main(){"
        "gl_FragData[0]=texture2D(tex,gl_TexCoord[0].xy)*gl_Color;}";
    const char *vbo_vertex = "#version 120\nattribute vec4 vs_in0;attribute vec4 vs_in5;"
        "attribute vec2 vs_in7;void main(){gl_Position=vs_in0;gl_FrontColor=vs_in5;"
        "gl_BackColor=vs_in5;gl_TexCoord[0]=vec4(vs_in7,0,1);}";
    static const struct packed_vertex { float xyzw[4]; unsigned char bgra[4]; float uv[2]; } vertices[] = {
        {{-1,-1,0,1},{191,64,128,128},{0,0}},
        {{ 1,-1,0,1},{191,64,128,128},{1,0}},
        {{-1, 1,0,1},{191,64,128,128},{0,1}},
        {{ 1, 1,0,1},{191,64,128,128},{1,1}},
    };
    static const unsigned char texel[] = {51,102,153,255};
    static const struct { const char *name; unsigned char expected[3]; int modulate, blend; } tests[] = {
        {"GLSL120_texture", {51,102,153}, 0, 0},
        {"GLSL120_diffuse", {26,26,115}, 1, 0},
        {"GLSL120_separate_alpha_blend", {13,13,57}, 1, 1},
    };
    GLuint vs = 0, fs = 0, program = 0, texture = 0;
    GLint ok = 0;
    unsigned i;
    char log[512];
    if (!create_shader || !shader_source || !compile_shader || !shader_iv ||
        !create_program || !attach_shader || !link_program || !program_iv || !use_program ||
        !uniform_location || !uniform1i || !delete_program || !delete_shader ||
        !blend_func_separate || !blend_equation_separate) {
        fprintf(out,"legacyShaderSetup\tmissing_entrypoint\t0\n");
        return;
    }
    while (glGetError() != GL_NO_ERROR) {}
    vs = create_shader(GL_VERTEX_SHADER); fs = create_shader(GL_FRAGMENT_SHADER);
    shader_source(vs,1,&vertex,NULL); shader_source(fs,1,&fragment,NULL);
    compile_shader(vs); compile_shader(fs);
    shader_iv(vs,GL_COMPILE_STATUS,&ok);
    if (!ok) {
        if (shader_log) { shader_log(vs,sizeof(log),NULL,log); fprintf(stderr,"GLSL120 vertex: %s\n",log); }
        goto cleanup;
    }
    shader_iv(fs,GL_COMPILE_STATUS,&ok);
    if (!ok) {
        if (shader_log) { shader_log(fs,sizeof(log),NULL,log); fprintf(stderr,"GLSL120 fragment: %s\n",log); }
        goto cleanup;
    }
    program = create_program(); attach_shader(program,vs); attach_shader(program,fs); link_program(program);
    program_iv(program,GL_LINK_STATUS,&ok);
    if (!ok) goto cleanup;
    glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,texel);
    glViewport(0,0,256,256); glDisable(GL_DEPTH_TEST); glDisable(GL_DITHER);
    use_program(program); uniform1i(uniform_location(program,"tex"),0);
    for (i=0;i<sizeof(tests)/sizeof(tests[0]);++i) {
        unsigned char pixel[4] = {0};
        unsigned error;
        int channel, passed = 1;
        glDisable(GL_BLEND); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        if (tests[i].modulate) glColor4f(0.5f,0.25f,0.75f,0.5f);
        else glColor4f(1,1,1,1);
        if (tests[i].blend) {
            glEnable(GL_BLEND);
            blend_func_separate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE);
            blend_equation_separate(GL_FUNC_ADD,GL_MAX);
        }
        glBegin(GL_QUADS);
        glTexCoord2f(0,0); glVertex2f(-1,-1);
        glTexCoord2f(1,0); glVertex2f(1,-1);
        glTexCoord2f(1,1); glVertex2f(1,1);
        glTexCoord2f(0,1); glVertex2f(-1,1);
        glEnd();
        glReadPixels(128,128,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel); error = glGetError();
        for (channel=0;channel<3;++channel)
            if (abs((int)pixel[channel]-(int)tests[i].expected[channel]) > 2) passed = 0;
        fprintf(out,"sample\t%s\t%u,%u,%u,%u\t%u,%u,%u,-\t%d\t%u\n",tests[i].name,
            pixel[0],pixel[1],pixel[2],pixel[3],tests[i].expected[0],tests[i].expected[1],
            tests[i].expected[2],passed && error == 0,error);
    }
    glColor4f(1,1,1,1); glDisable(GL_BLEND); blend_equation_separate(GL_FUNC_ADD,GL_FUNC_ADD);
    if (bind_attrib && gen_buffers && bind_buffer && buffer_data && map_buffer && unmap_buffer &&
        delete_buffers && attrib_pointer && enable_attrib && disable_attrib) {
        GLuint buffer;
        use_program(0); shader_source(vs,1,&vbo_vertex,NULL); compile_shader(vs);
        shader_iv(vs,GL_COMPILE_STATUS,&ok);
        bind_attrib(program,0,"vs_in0"); bind_attrib(program,5,"vs_in5"); bind_attrib(program,7,"vs_in7");
        link_program(program); program_iv(program,GL_LINK_STATUS,&ok);
        if (!ok) goto cleanup;
        use_program(program); uniform1i(uniform_location(program,"tex"),0);
        gen_buffers(1,&buffer); bind_buffer(GL_ARRAY_BUFFER,buffer);
        enable_attrib(0); enable_attrib(5); enable_attrib(7);
        attrib_pointer(0,4,GL_FLOAT,GL_FALSE,sizeof(vertices[0]),(const void *)0);
        attrib_pointer(5,GL_BGRA,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(vertices[0]),(const void *)16);
        attrib_pointer(7,2,GL_FLOAT,GL_FALSE,sizeof(vertices[0]),(const void *)20);
        for (i=0;i<2;++i) {
            unsigned char pixel[4] = {0};
            unsigned error;
            int passed = 1;
            buffer_data(GL_ARRAY_BUFFER,sizeof(vertices),i ? NULL : vertices,GL_STREAM_DRAW);
            if (i) {
                void *mapped = map_buffer(GL_ARRAY_BUFFER,GL_WRITE_ONLY);
                if (!mapped) passed = 0;
                else { memcpy(mapped,vertices,sizeof(vertices)); if (!unmap_buffer(GL_ARRAY_BUFFER)) passed = 0; }
            }
            glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            glReadPixels(128,128,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel); error = glGetError();
            if (abs((int)pixel[0]-26)>2 || abs((int)pixel[1]-26)>2 || abs((int)pixel[2]-115)>2) passed = 0;
            fprintf(out,"sample\tGLSL120_BGRA_%sVBO\t%u,%u,%u,%u\t26,26,115,-\t%d\t%u\n",
                i ? "mapped_" : "",pixel[0],pixel[1],pixel[2],pixel[3],passed && error == 0,error);
        }
        disable_attrib(0); disable_attrib(5); disable_attrib(7);
        bind_buffer(GL_ARRAY_BUFFER,0); delete_buffers(1,&buffer);
    } else fprintf(out,"legacyVboSetup\tmissing_entrypoint\t0\n");
    use_program(0); glBindTexture(GL_TEXTURE_2D,0); glDeleteTextures(1,&texture);
cleanup:
    if (!ok) fprintf(out,"legacyShaderSetup\tcompile_or_link_failure\t0\n");
    if (program) delete_program(program);
    if (vs) delete_shader(vs);
    if (fs) delete_shader(fs);
}

static int shader_triangle(int version, unsigned char pixel[4], unsigned *error)
{
    PFNGLCREATESHADERPROC create_shader = (void *)wglGetProcAddress("glCreateShader");
    PFNGLSHADERSOURCEPROC shader_source = (void *)wglGetProcAddress("glShaderSource");
    PFNGLCOMPILESHADERPROC compile_shader = (void *)wglGetProcAddress("glCompileShader");
    PFNGLGETSHADERIVPROC shader_iv = (void *)wglGetProcAddress("glGetShaderiv");
    PFNGLGETSHADERINFOLOGPROC shader_log = (void *)wglGetProcAddress("glGetShaderInfoLog");
    PFNGLCREATEPROGRAMPROC create_program = (void *)wglGetProcAddress("glCreateProgram");
    PFNGLATTACHSHADERPROC attach_shader = (void *)wglGetProcAddress("glAttachShader");
    PFNGLLINKPROGRAMPROC link_program = (void *)wglGetProcAddress("glLinkProgram");
    PFNGLGETPROGRAMIVPROC program_iv = (void *)wglGetProcAddress("glGetProgramiv");
    PFNGLUSEPROGRAMPROC use_program = (void *)wglGetProcAddress("glUseProgram");
    PFNGLGENVERTEXARRAYSPROC gen_vao = (void *)wglGetProcAddress("glGenVertexArrays");
    PFNGLBINDVERTEXARRAYPROC bind_vao = (void *)wglGetProcAddress("glBindVertexArray");
    PFNGLDELETEVERTEXARRAYSPROC delete_vao = (void *)wglGetProcAddress("glDeleteVertexArrays");
    PFNGLDELETEPROGRAMPROC delete_program = (void *)wglGetProcAddress("glDeleteProgram");
    PFNGLDELETESHADERPROC delete_shader = (void *)wglGetProcAddress("glDeleteShader");
    char vertex[512], fragment[256], log[1024];
    const char *v = vertex, *f = fragment;
    GLuint vs, fs, program, vao;
    GLint ok = 0;
    int result = 0;
    if (!create_shader || !shader_source || !compile_shader || !shader_iv ||
        !create_program || !attach_shader || !link_program || !program_iv ||
        !use_program || !gen_vao || !bind_vao || !delete_vao || !delete_program || !delete_shader)
        return 0;
    snprintf(vertex, sizeof(vertex), "#version %d\nvoid main(){"
        "vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
        "gl_Position=vec4(p[gl_VertexID],0,1);}", version);
    snprintf(fragment, sizeof(fragment), "#version %d\nout vec4 color;"
        "void main(){color=vec4(0.2,0.4,0.6,1.0);}", version);
    vs = create_shader(GL_VERTEX_SHADER);
    fs = create_shader(GL_FRAGMENT_SHADER);
    shader_source(vs, 1, &v, NULL);
    shader_source(fs, 1, &f, NULL);
    compile_shader(vs);
    compile_shader(fs);
    shader_iv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok && shader_log) { shader_log(vs, sizeof(log), NULL, log); fprintf(stderr,"WGL-PROBE vertex: %s\n",log); }
    if (!ok) goto cleanup;
    shader_iv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok && shader_log) { shader_log(fs, sizeof(log), NULL, log); fprintf(stderr,"WGL-PROBE fragment: %s\n",log); }
    if (!ok) goto cleanup;
    program = create_program();
    attach_shader(program, vs);
    attach_shader(program, fs);
    link_program(program);
    program_iv(program, GL_LINK_STATUS, &ok);
    if (!ok) { delete_program(program); goto cleanup; }
    gen_vao(1, &vao);
    bind_vao(vao);
    use_program(program);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(128,128,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    *error = glGetError();
    result = *error == GL_NO_ERROR && pixel[0] >= 50 && pixel[0] <= 52 &&
        pixel[1] >= 101 && pixel[1] <= 103 && pixel[2] >= 152 && pixel[2] <= 154;
    use_program(0);
    bind_vao(0);
    delete_vao(1,&vao);
    delete_program(program);
cleanup:
    delete_shader(vs);
    delete_shader(fs);
    return result;
}

int main(int argc, char **argv)
{
    static const struct { int major, minor, profile, flags; } cases[] = {
        {2,1,0,0}, {3,0,0,0}, {3,1,0,0}, {3,2,0,0}, {3,2,0,2}, {3,2,1,0},
        {3,3,1,0}, {3,3,2,0}, {4,0,1,0}, {4,1,1,0}, {4,2,1,0}, {4,3,1,0},
        {4,4,1,0}, {4,5,1,0}, {4,6,1,0}, {4,6,2,0},
    };
    const char *output = argc > 1 ? argv[1] : "C:\\vp-chaos-wgl-capabilities.tsv";
    WNDCLASSA wc = {0};
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
    HWND hwnd;
    HDC dc;
    HGLRC bootstrap;
    create_attribs_fn create;
    FILE *out = fopen(output, "w");
    unsigned i;
    int succeeded = 0;
    if (!out) return 2;
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = "VPWglCapabilityProbe";
    wc.hInstance = GetModuleHandleA(NULL);
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    hwnd = CreateWindowA(wc.lpszClassName,"WGL capability probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        20,20,420,350,NULL,NULL,wc.hInstance,NULL);
    dc = GetDC(hwnd);
    if (!dc || !SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd) ||
        !(bootstrap = wglCreateContext(dc)) || !wglMakeCurrent(dc, bootstrap)) return 3;
    create = (create_attribs_fn)wglGetProcAddress("wglCreateContextAttribsARB");
    fprintf(out,"requested\tprofile\tflags\tcreated\twinerr\tactualGL\tGLSL\tclearRGBA\tclearOK\tshaderRGBA\tshaderOK\tglerr\n");
    for (i=0; i<sizeof(cases)/sizeof(cases[0]); ++i) {
        int a[9] = {WGL_CONTEXT_MAJOR_VERSION_ARB,cases[i].major,WGL_CONTEXT_MINOR_VERSION_ARB,cases[i].minor};
        unsigned n = 4;
        unsigned char p[4]={0}, q[4]={0};
        unsigned error = 0;
        int clear_ok = 0, shader_ok = 0;
        DWORD winerr;
        HGLRC ctx;
        if (cases[i].profile) { a[n++] = WGL_CONTEXT_PROFILE_MASK_ARB; a[n++] = cases[i].profile; }
        if (cases[i].flags) { a[n++] = WGL_CONTEXT_FLAGS_ARB; a[n++] = cases[i].flags; }
        a[n] = 0;
        SetLastError(0);
        ctx = i == 0 ? wglCreateContext(dc) : create ? create(dc,NULL,a) : NULL;
        winerr = GetLastError();
        if (!ctx || !wglMakeCurrent(dc,ctx)) {
            fprintf(out,"%d.%d\t%d\t%d\t0\t%lu\t-\t-\t-\t0\t-\t0\t0\n",
                cases[i].major,cases[i].minor,cases[i].profile,cases[i].flags,winerr);
            if (ctx) wglDeleteContext(ctx);
            fflush(out); continue;
        }
        while (glGetError() != GL_NO_ERROR) {}
        glViewport(0,0,256,256);
        glDisable(GL_DITHER);
        glClearColor(0.2f,0.4f,0.6f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glReadPixels(128,128,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
        error=glGetError();
        clear_ok = error == 0 && p[0]>=50 && p[0]<=52 && p[1]>=101 && p[1]<=103 && p[2]>=152 && p[2]<=154;
        if (i) {
            int glsl = cases[i].major == 3 && cases[i].minor < 3 ?
                130 + 10 * cases[i].minor : 100 * cases[i].major + 10 * cases[i].minor;
            shader_ok=shader_triangle(glsl,q,&error);
        }
        fprintf(out,"%d.%d\t%d\t%d\t1\t%lu\t%s\t%s\t%u,%u,%u,%u\t%d\t%u,%u,%u,%u\t%d\t%u\n",
            cases[i].major,cases[i].minor,cases[i].profile,cases[i].flags,winerr,
            glGetString(GL_VERSION),glGetString(GL_SHADING_LANGUAGE_VERSION),p[0],p[1],p[2],p[3],clear_ok,
            q[0],q[1],q[2],q[3],shader_ok,error);
        fflush(out);
        if (!i) {
            GLint texture_size = 0, ubo_size = 0, ubo_count = 0;
            fprintf(out,"legacyExtensions\t%s\n",glGetString(GL_EXTENSIONS));
            fprintf(out,"legacyRenderer\t%s\n",glGetString(GL_RENDERER));
            fprintf(out,"legacyVendor\t%s\n",glGetString(GL_VENDOR));
            glGetIntegerv(GL_MAX_TEXTURE_SIZE,&texture_size);
            glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE,&ubo_size);
            glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS,&ubo_count);
            fprintf(out,"legacyLimits\tmaxTextureSize=%d\tmaxUniformBlockSize=%d\tmaxVertexUniformBlocks=%d\n",
                texture_size,ubo_size,ubo_count);
            texture_samples(out);
            legacy_shader_samples(out);
        }
        SwapBuffers(dc);
        wglMakeCurrent(NULL,NULL);
        wglDeleteContext(ctx);
        wglMakeCurrent(dc,bootstrap);
        ++succeeded;
    }
    fprintf(stderr,"WGL-PROBE: %d contexts created, result %s\n",succeeded,output);
    fclose(out);
    wglMakeCurrent(NULL,NULL);
    wglDeleteContext(bootstrap);
    ReleaseDC(hwnd,dc);
    DestroyWindow(hwnd);
    return succeeded ? 0 : 4;
}
