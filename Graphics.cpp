
//
// Graphics.cpp - drawing routines
// 
// Created on 10/31/12.
// 

// Copyright (c) 2013-2015 Arthur Danskin
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


#include "StdAfx.h"
#include "Graphics.h"
#include "Shaders.h"
#include "Event.h"

#ifndef ASSERT_MAIN_THREAD
#define ASSERT_MAIN_THREAD()
#endif

template struct TriMesh<VertexPosColor>;
template struct LineMesh<VertexPosColor>;
template struct MeshPair<VertexPosColor, VertexPosColor>;

bool supports_ARB_Framebuffer_object = false;

static const uint kDebugFrames = 10;

bool isGLExtensionSupported(const char *name)
{
    static const GLubyte* val = glGetString(GL_EXTENSIONS);
    static const string str((const char*)val);
    return str.find(name) != std::string::npos;
}

uint graphicsDrawCount = 0;
uint gpuMemoryUsed = 0;

void deleteBufferInMainThread(GLuint buffer)
{
    globals.deleteGLBuffers(1, &buffer);
}


GLenum glReportError1(const char *file, uint line, const char *function)
{
    ASSERT_MAIN_THREAD();

    if (!(globals.debugRender&DBG_GLERROR) && globals.frameStep > kDebugFrames)
        return GL_NO_ERROR;

	GLenum err = GL_NO_ERROR;
	while (GL_NO_ERROR != (err = glGetError()))
    {
        const char* msg = (const char *)gluErrorString(err);
        OLG_OnAssertFailed(file, line, function, "glGetError", "%s", msg);
    }
    
	return err;
}


static string getGLFrameBufferStatusString(GLenum err)
{
    switch(err) {
        case 0: return "Error checking framebuffer status";
        CASE_STR(GL_FRAMEBUFFER_COMPLETE);
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);
        CASE_STR(GL_FRAMEBUFFER_UNSUPPORTED);
#if OPENGL_ES
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS);
#else
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_EXT);
        CASE_STR(GL_FRAMEBUFFER_UNDEFINED);
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER);
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER);
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE);
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS_EXT);
        CASE_STR(GL_FRAMEBUFFER_INCOMPLETE_LAYER_COUNT_EXT);
#endif
        default: return str_format("0x%0x", err);
    }
}

static GLenum glReportFramebufferError1(const char *file, uint line, const char *function)
{
    ASSERT_MAIN_THREAD();

    if (!(globals.debugRender&DBG_GLERROR) && globals.frameStep > kDebugFrames)
        return GL_NO_ERROR;

	GLenum err = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (GL_FRAMEBUFFER_COMPLETE != err)
    {
        OLG_OnAssertFailed(file, line, function, "glCheckFramebufferStatus", "%s", getGLFrameBufferStatusString(err).c_str());
    }
    
	return err;
}

#define glReportFramebufferError() glReportFramebufferError1(__FILE__, __LINE__, __func__)

static bool ignoreShaderLog(const char* buf)
{
    // damnit ATI driver
    static string kNoErrors = "No errors.\n";
    return (buf == kNoErrors ||
            str_contains(buf, "Validation successful") ||
            str_contains(buf, "successfully compiled") ||
            str_contains(buf, "shader(s) linked."));
}

static void checkProgramInfoLog(GLuint prog, const char* name)
{
    const uint bufsize = 2048;
    char buf[bufsize];
    GLsizei length = 0;
    glGetProgramInfoLog(prog, bufsize, &length, buf);
    if (length && !ignoreShaderLog(buf))
    {
        ASSERT(length < bufsize);
        OLG_OnAssertFailed(name, -1, "", "", "GL Program Info log for '%s': %s", name, buf);
    }
}

void glReportValidateShaderError1(const char *file, uint line, const char *function, GLuint program, const char *name)
{
    ASSERT_MAIN_THREAD();

    if (!(globals.debugRender&DBG_GLERROR) && globals.frameStep > kDebugFrames)
        return;

    glValidateProgram(program);
    GLint status = 0;
    glGetProgramiv(program, GL_VALIDATE_STATUS, &status);
    checkProgramInfoLog(program, "validate");
    glReportError1(file, line, function);
    ASSERT_(status == GL_TRUE, file, line, function, "%s", name);
}


vector<GLRenderTexture*> GLRenderTexture::s_bound;

GLRenderTexture* GLRenderTexture::getBound(int idx)
{
    return vec_at(s_bound, -idx - 1);
}

static const char* textureFormatToString(GLint fmt)
{
    switch (fmt) {
        CASE_STR(GL_RGB);
        CASE_STR(GL_RGBA);
        CASE_STR(GL_BGRA);
#if OPENGL_ES
        CASE_STR(GL_RGB16F_EXT);
#else
        CASE_STR(GL_BGR);
        CASE_STR(GL_RGBA16F_ARB);
        CASE_STR(GL_RGB16F_ARB);
#endif
        default: return "<unknown>";
    }
}

static int textureFormatBytesPerPixel(GLint fmt)
{
    switch (fmt) {
    case GL_RGB16F_ARB:
    case GL_RGBA16F_ARB: return 2 * 4;
    default:
        return 4;
    }
}

static GLint s_defaultFramebuffer = -1;

void GLRenderTexture::Generate(ZFlags zflags)
{
    ASSERT_MAIN_THREAD();

    ASSERT(m_size.x >= 1 && m_size.y >= 1);

    GLsizei width = m_size.x;
    GLsizei height = m_size.y;
    
#if OPENGL_ES
    // textures must be a power of 2 on ios
    width = roundUpPower2(width);
    height = roundUpPower2(height);
#endif
    
    if (s_defaultFramebuffer < 0)
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_defaultFramebuffer);
    }

    m_texsize = float2(width, height);
    DPRINT(SHADER, ("Generating render texture, %dx%d %s %s", width, height,
                    textureFormatToString(m_format), (zflags&HASZ) ? "Z16" : "No_Z"));

    glReportError();
    glGenFramebuffers(1, &m_fbname);
    glReportError();

    glGenTextures(1, &m_texname);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texname);
    glReportError();
 
#if OPENGL_ES
    if (m_format == GL_RGBA16F_ARB)
    {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_HALF_FLOAT_OES, 0);
    }
    else
#endif
    {
        glTexImage2D(GL_TEXTURE_2D, 0, m_format, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
    }
    glReportError();
    gpuMemoryUsed += width * height * textureFormatBytesPerPixel(m_format);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbname);
    glReportError();

    // The depth buffer
    if (zflags&HASZ)
    {
        glGenRenderbuffers(1, &m_zrbname);
        glReportError();
        glBindRenderbuffer(GL_RENDERBUFFER, m_zrbname);
        glReportError();
        
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
        glReportError();
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_zrbname);
        glReportError();

        gpuMemoryUsed += width * height * 2;
    }
    else
    {
        m_zrbname = 0;
    }

    // Set "renderedTexture" as our colour attachement #0
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texname, 0);
    glReportError();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texname);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

#if 0 
    // Set the list of draw buffers.
    GLenum DrawBuffers[2] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, DrawBuffers); // "1" is the size of DrawBuffers
#endif

    // Always check that our framebuffer is ok
    glReportError();
    glReportFramebufferError();
}

void GLRenderTexture::clear()
{
    if (m_fbname)
        globals.deleteGLFramebuffers(1, &m_fbname);
    if (m_texname) {
        gpuMemoryUsed -= m_size.x * m_size.y * textureFormatBytesPerPixel(m_format);
        globals.deleteGLTextures(1, &m_texname);
    }
    if (m_zrbname) {
        gpuMemoryUsed -= m_size.x * m_size.y * 2;
        globals.deleteGLRenderbuffers(1, &m_zrbname);
    }
    m_size = float2(0.f);
    m_texsize = float2(0.f);
    m_texname = 0;
    m_fbname = 0;
    m_zrbname = 0;
}

#if __APPLE__
#define HAS_BLIT_FRAMEBUFFER 1
#else
#define HAS_BLIT_FRAMEBUFFER glBlitFramebuffer
#endif

void GLRenderTexture::BindFramebuffer(float2 size, ZFlags zflags)
{
    ASSERT(!isZero(size));
    if (size != m_size || ((zflags&HASZ) && !(m_zflags&HASZ)))
        clear();
    m_size = size;
    m_zflags = zflags;
    if (!m_fbname)
        Generate(zflags);
    RebindFramebuffer();

#if !OPENGL_ES
    const GLint def = s_bound.size() ? s_bound.back()->m_fbname : s_defaultFramebuffer;
    if ((zflags == KEEPZ) && HAS_BLIT_FRAMEBUFFER && def >= 0)
    {
        ASSERT(def != m_fbname);
        const float2 lastSize = s_bound.size() ? s_bound.back()->m_size : globals.windowSizePixels;
        ASSERT(lastSize.x > 0.f && lastSize.y > 0.f);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, def);
        glReportError();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbname);
        glReportError();
        // only GL_NEAREST is supported for depth buffers
        glBlitFramebuffer(0, 0, lastSize.x, lastSize.y, 0, 0, m_size.x, m_size.y,
                          GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glReportError();
    }
    else
#endif
    if (zflags&HASZ)
    {
        glClear(GL_DEPTH_BUFFER_BIT);
        glReportError();
    }
    
    ASSERT(s_bound.empty() || s_bound.back() != this);
    s_bound.push_back(this);
}

void GLRenderTexture::RebindFramebuffer()
{
    ASSERT(m_size.x >= 1 && m_size.y >= 1);
    ASSERT(m_fbname && m_texname);

    BindTexture(0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbname);
    glReportFramebufferError();
    glViewport(0,0,m_size.x,m_size.y);
    glReportError();
}

void GLRenderTexture::UnbindFramebuffer() const
{
    ASSERT(s_bound.size() && s_bound.back() == this);
    s_bound.pop_back();
    
    if (s_bound.size())
    {
        s_bound.back()->RebindFramebuffer();
    }
    else
    {
        glReportFramebufferError();
        if (s_defaultFramebuffer >= 0)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, s_defaultFramebuffer);
            glReportError();
            glReportFramebufferError();
            glViewport(0, 0, globals.windowSizePixels.x, globals.windowSizePixels.y);
            glReportError();
        }
    }
}

void GLTexture::clear()
{
    if (m_texname)
    {
        gpuMemoryUsed -= m_texsize.x * m_texsize.y * textureFormatBytesPerPixel(m_format);
        globals.deleteGLTextures(1, &m_texname);
    }
    m_texname = 0;
}

void GLTexture::DrawFSBegin(ShaderState& ss) const
{
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
#if !OPENGL_ES
    glDisable(GL_ALPHA_TEST);
#endif

    ss.uTransform = glm::ortho(0.f, 1.f, 0.f, 1.f, -1.f, 1.f);
}

void GLTexture::DrawFSEnd() const
{
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
#if !OPENGL_ES
    glEnable(GL_ALPHA_TEST);
#endif
    glReportError();
}

void GLTexture::TexImage2D(GLenum int_format, int2 size, GLenum format, GLenum type, const void *data)
{
    m_format = int_format;
    if (!m_texname) {
        glGenTextures(1, &m_texname);
    } else {
        gpuMemoryUsed -= m_texsize.x * m_texsize.y * textureFormatBytesPerPixel(m_format);
    }
    glBindTexture(GL_TEXTURE_2D, m_texname);

#if OPENGL_ES
    m_texsize = float2(roundUpPower2(size.x), roundUpPower2(size.y));
    glTexImage2D(GL_TEXTURE_2D, 0, format,
                 m_texsize.x, m_texsize.y, 0, format, GL_UNSIGNED_BYTE, data);
    glReportError();
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
    m_texsize = float2(size);
    glTexImage2D(GL_TEXTURE_2D, 0, m_format,
                 size.x, size.y, 0, format, type, data);
    glReportError();

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_SGIS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_SGIS);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, 0);
    m_size = float2(size);
    gpuMemoryUsed += m_texsize.x * m_texsize.y * textureFormatBytesPerPixel(m_format);
}

void GLTexture::SetTexWrap(bool enable)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texname);
    const GLint param = enable ? GL_REPEAT :  GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, param);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, param);
}

void GLTexture::SetTexMagFilter(GLint filter)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texname);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

void GLTexture::GenerateMipmap()
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texname);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);
    glReportError();
}

bool GLTexture::loadFile(const char* fname)
{
    clear();
    OutlawImage image = OL_LoadImage(fname);
    if (!image.data)
        return false;
    
    glGenTextures(1, &m_texname);
    glBindTexture(GL_TEXTURE_2D, m_texname);
    //glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP_SGIS, GL_TRUE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, image.format, image.type, image.data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE_SGIS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE_SGIS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glGenerateMipmap(GL_TEXTURE_2D);

    glReportError();
    
    OL_FreeImage(&image);
    
    m_size.x  = image.width;
    m_size.y  = image.height;
    m_texsize = m_size;
    m_format  = GL_RGBA;
    return true;
}

static void invert_image(uint *pix, int width, int height)
{
    for (int y=0; y<height/2; y++)
    {
        for (int x=0; x<width; x++)
        {
            const int top = y * width + x;
            const int bot = (height - y - 1) * width + x;
            const uint temp = pix[top];
            pix[top] = pix[bot];
            pix[bot] = temp;
        }
    }
}

bool GLTexture::writeFile(const char *fname) const
{
    const int2 sz = ceil_int(m_texsize);
    const size_t size = sz.x * sz.y * 4;
    uint *pix = (uint*) malloc(size);

    OutlawImage img = {};
    img.width = sz.x;
    img.height = sz.y;
    img.format = GL_RGBA;
    img.type = GL_UNSIGNED_BYTE;
    img.data = (char*) pix;

    BindTexture(0);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    glReportError();

    invert_image(pix, sz.x, sz.y);

    const int success = OL_SaveImage(&img, fname);
    free(pix);
    return success;
}


GLTexture PixImage::uploadTexture() const
{
    GLTexture tex;
    tex.TexImage2D(GL_RGBA, m_size, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &m_data[0]);
    return tex;
}

ShaderProgramBase::ShaderProgramBase()
{
}

ShaderProgramBase::~ShaderProgramBase()
{
    reset(); 
}

GLuint ShaderProgramBase::createShader(const char* txt, GLenum type) const
{
    GLuint idx = glCreateShader(type);
    glShaderSource(idx, 1, &txt, NULL);
    glCompileShader(idx);

    {
        const uint bufsize = 2048;
        char buf[bufsize];
        GLsizei length = 0;
        glGetShaderInfoLog(idx, bufsize, &length, buf);
        if (length && !ignoreShaderLog(buf)) {
            ASSERT(length < bufsize);
            OLG_OnAssertFailed(m_name.c_str(), -1, "", "glCompileShader", "GL Shader Info Log:\n%s\n%s",
                               str_add_line_numbers(txt).c_str(), buf);
        }
    }
    
    GLint val = 0;
    glGetShaderiv(idx, GL_COMPILE_STATUS, &val);
    if (val == GL_FALSE) {
        glDeleteShader(idx);
        return 0;
    }
    
    return idx;
}

GLint ShaderProgramBase::getAttribLocation(const char *name) const
{
    if (!isLoaded())
        return -1;
    GLint v = glGetAttribLocation(m_programHandle, name);
    ASSERTF(v >= 0, "%s::%s", m_name.c_str(), name);
    glReportError();
    return v;
}

GLint ShaderProgramBase::getUniformLocation(const char *name) const
{
    if (!isLoaded())
        return -1;
    GLint v = glGetUniformLocation(m_programHandle, name);
    ASSERTF(v >= 0, "%s::%s", m_name.c_str(), name);
    glReportError();
    return v;
}

void ShaderProgramBase::reset()
{
    ASSERT_MAIN_THREAD();
    if (m_programHandle) {
        glDeleteProgram(m_programHandle);
        m_programHandle = 0;
        m_name = "";
    }
}

bool ShaderProgramBase::LoadProgram(const char* name, const char* shared, const char *vertf, const char *fragf)
{
    ASSERT_MAIN_THREAD();

    m_name = name;
    DPRINT(SHADER, ("Compiling %s(%s)", name, m_argstr.c_str()));
    
    string header =
#if OPENGL_ES
        "precision highp float;\n"
        "precision highp sampler2D;\n"
#else
        "#version 120\n"
#endif
         "#define M_PI 3.1415926535897932384626433832795\n";

    header += m_header + "\n";
    header += shared;
    header += "\n";
    static const char* vertheader =
        "attribute vec4 Position;\n"
        "uniform mat4 Transform;\n";
    
    string vertful = header + vertheader + vertf;
    string fragful = header + fragf;

    GLuint vert = createShader(vertful.c_str(), GL_VERTEX_SHADER);
    GLuint frag = createShader(fragful.c_str(), GL_FRAGMENT_SHADER);

    if (!vert || !frag)
        return false;

    if (m_programHandle) {
        DPRINT(SHADER, ("Deleting old %s", name));
        glDeleteProgram(m_programHandle);
    }
    
    m_programHandle = glCreateProgram();
    ASSERTF(m_programHandle, "%s", m_name.c_str());
    if (!m_programHandle)
        return false;
    glGetError();
    glAttachShader(m_programHandle, vert);
    glReportError();
    glAttachShader(m_programHandle, frag);
    glReportError();
    
    glLinkProgram(m_programHandle);
    glReportError();

    glDeleteShader(vert);
    glReportError();
    glDeleteShader(frag);
    glReportError();

    checkProgramInfoLog(m_programHandle, name);
    
    GLint linkSuccess = 0;
    glGetProgramiv(m_programHandle, GL_LINK_STATUS, &linkSuccess);
    if (linkSuccess == GL_FALSE) {
        DPRINT(SHADER, ("Compiling %s failed", name));
        glDeleteProgram(m_programHandle);
        m_programHandle = 0;
        return false;
    }
    
    m_positionSlot = glGetAttribLocation(m_programHandle, "Position");
    glReportError();
    
    m_transformUniform = glGetUniformLocation(m_programHandle, "Transform");
    glReportError();
    return true;
}

void ShaderProgramBase::UseProgramBase(const ShaderState& ss, uint size, const float3* pos) const
{
    UseProgramBase(ss);
    if (m_positionSlot >= 0) {
        glEnableVertexAttribArray(m_positionSlot);
        vap1(m_positionSlot, size, pos);
        glReportError();
    }
}

void ShaderProgramBase::UseProgramBase(const ShaderState& ss, uint size, const float2* pos) const
{
    UseProgramBase(ss);
    if (m_positionSlot >= 0) {
        glEnableVertexAttribArray(m_positionSlot);
        vap1(m_positionSlot, size, pos);
        glReportError();
    }
}

void ShaderProgramBase::UseProgramBase(const ShaderState& ss) const
{
    ASSERT_MAIN_THREAD();
    ASSERTF(isLoaded(), "%s", m_name.c_str());
    glReportError();
    glUseProgram(m_programHandle);
    glUniformMatrix4fv(m_transformUniform, 1, GL_FALSE, &ss.uTransform[0][0]);
    glReportError();
}

void ShaderProgramBase::UnuseProgram() const
{
    ASSERT_MAIN_THREAD();
    glReportValidateShaderError(m_programHandle, m_name.c_str());
    if (m_positionSlot >= 0) {
        glDisableVertexAttribArray(m_positionSlot);
    }
    foreach (GLuint slot, m_enabledAttribs)
        glDisableVertexAttribArray(slot);
    m_enabledAttribs.clear();
    glUseProgram(0);
}

void ShaderState::DrawElements(GLenum dt, size_t ic, const ushort* i) const
{
    ASSERT_MAIN_THREAD();
    glDrawElements(dt, (GLsizei) ic, GL_UNSIGNED_SHORT, i);
    glReportError();
    graphicsDrawCount++;
}

void ShaderState::DrawElements(uint dt, size_t ic, const uint* i) const
{
    ASSERT_MAIN_THREAD();
    glDrawElements(dt, (GLsizei) ic, GL_UNSIGNED_INT, i);
    glReportError();
    graphicsDrawCount++;
}

void ShaderState::DrawArrays(uint dt, size_t count) const
{
    ASSERT_MAIN_THREAD();
    glDrawArrays(dt, 0, (GLsizei)count);
    glReportError();
    graphicsDrawCount++;
}


void DrawAlignedGrid(ShaderState &wss, const View& view, float size, float z)
{
    const double2 roundedCam  = double2(round(view.position, size));
    const double2 roundedSize = double2(round(0.5f * view.getWorldSize(z), size) + float2(size));
    ShaderUColor::instance().DrawGrid(wss, size, 
                                      double3(roundedCam - roundedSize, z), 
                                      double3(roundedCam + roundedSize, z));
} 


void ShaderPosBase::DrawGrid(const ShaderState &ss_, double width, double3 first, double3 last) const
{
    ShaderState ss = ss_;
    ss.translateZ(first.z);

    uint xCount = ceil_int((last.x - first.x) / width);
    uint yCount = ceil_int((last.y - first.y) / width);

    float2 *v = new float2[2 * (xCount + yCount)];
    float2 *pv = v;

    for (uint x=0; x<xCount; x++)
    {
        pv->x = first.x + x * width;
        pv->y = first.y;
        pv++;
        pv->x = first.x + x * width;
        pv->y = last.y;
        pv++;
    }

    for (uint y=0; y<yCount; y++)
    {
        pv->x = first.x;
        pv->y = first.y + y * width;
        pv++;
        pv->x = last.x;
        pv->y = first.y + y * width;
        pv++;
    }

    UseProgram(ss, v);
    ss.DrawArrays(GL_LINES, 2 * (xCount + yCount));
    UnuseProgram();
    delete[] v;
}

void PushButton(TriMesh<VertexPosColor>* triP, LineMesh<VertexPosColor>* lineP, float2 pos, float2 r, uint bgColor, uint fgColor, float alpha)
{
    static const float o = 0.1f;
    const float2 v[6] = { pos + float2(-r.x, lerp(r.y, -r.y, o)),
                          pos + float2(lerp(-r.x, r.x, o), r.y),
                          pos + float2(r.x, r.y),
                          pos + float2(r.x, lerp(-r.y, r.y, o)),
                          pos + float2(lerp(r.x, -r.x, o), -r.y),
                          pos + float2(-r.x, -r.y) };

    if (triP && (bgColor&ALPHA_OPAQUE) && alpha > epsilon) {
        triP->color32(bgColor, alpha);
        triP->PushPoly(v, 6);
    }

    if (lineP && (fgColor&ALPHA_OPAQUE) && alpha > epsilon) {
        lineP->color32(fgColor, alpha);
        lineP->PushLoop(v, 6);
    }
}

void PushButton1(TriMesh<VertexPosColor>* triP, LineMesh<VertexPosColor>* lineP, float2 pos, float2 r, uint bgColor, uint fgColor, float alpha)
{
    static const float o = 0.1f;
    const float2 v[5] = { pos + float2(-r.x, r.y),
                          pos + float2(r.x, r.y),
                          pos + float2(r.x, lerp(-r.y, r.y, o)),
                          pos + float2(lerp(r.x, -r.x, o), -r.y),
                          pos + float2(-r.x, -r.y) };

    if (triP && (bgColor&ALPHA_OPAQUE) && alpha > epsilon) {
        triP->color32(bgColor, alpha);
        triP->PushPoly(v, 5);
    }

    if (lineP && (fgColor&ALPHA_OPAQUE) && alpha > epsilon) {
        lineP->color32(fgColor, alpha);
        lineP->PushLoop(v, 5);
    }
}

void DrawButton(const ShaderState *data, float2 pos, float2 r, uint bgColor, uint fgColor, float alpha)
{
    if (alpha < epsilon)
        return;
    DMesh::Handle h(theDMesh());
    PushButton(&h.mp.tri, &h.mp.line, pos, r, bgColor, fgColor, alpha);
    h.Draw(*data);
}

void DrawFilledRect(const ShaderState &s_, float2 pos, float2 rad, uint bgColor, uint fgColor, float alpha)
{
    ShaderState ss = s_;
    ss.translateZ(-1.f);
    ss.color32(bgColor, alpha);
    ShaderUColor::instance().DrawRect(ss, pos, rad);
    ss.color32(fgColor,alpha);
    ss.translateZ(0.1f);
    ShaderUColor::instance().DrawLineRect(ss, pos, rad);
}

float2 DrawBar(const ShaderState &s1, uint fill, uint line, float alpha, float2 p, float2 s, float a)
{
    ShaderState ss = s1;
    a = clamp(a, 0.f, 1.f);
    ss.color(fill, alpha);
    const float2 wp = p + float2(1.f, -1.f);
    const float2 ws = s - float2(2.f);
    ShaderUColor::instance().DrawQuad(ss, wp, wp + a * justX(ws), wp - justY(ws), wp + float2(a*ws.x, -ws.y));
    ss.color(line, alpha);
    ShaderUColor::instance().DrawLineQuad(ss, p, p + justX(s), p - justY(s), p + flipY(s));
    return s;
}


void PushRect(TriMesh<VertexPosColor>* triP, LineMesh<VertexPosColor>* lineP, float2 pos, float2 r, 
             uint bgColor, uint fgColor, float alpha)
{
    if (lineP) {
        lineP->color32(fgColor, alpha);
        lineP->PushRect(pos, r);
    }
    if (triP) {
        triP->color32(bgColor, alpha);
        triP->PushRect(pos, r);
    }
}

void fadeFullScreen(const ShaderState &s_, const View& view, uint color, float alpha)
{
    if (alpha < epsilon)
        return;
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    ShaderState ss = s_;
    ss.color(color, alpha);
    ShaderUColor::instance().DrawRectCorners(ss, -0.1f * view.sizePoints, 1.1f * view.sizePoints);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void sexyFillScreen(const ShaderState &ss, const View& view, uint color0, uint color1, float alpha)
{
    if (alpha < epsilon || (color0 == 0 && color1 == 0))
        return;

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    const float2 ws = 1.2f * view.sizePoints;
    const float2 ps = -0.1f * view.sizePoints;
    const float t = globals.renderTime / 20.f;
    const uint a = ALPHAF(alpha);

    // 1 2
    // 0 3
    const VertexPosColor v[] = {
        VertexPosColor(ps,  a|rgb2bgr(lerpXXX(color0, color1, unorm_sin(t)))),
        VertexPosColor(ps + justY(ws), a|rgb2bgr(lerpXXX(color0, color1, unorm_sin(3.f * t)))),
        VertexPosColor(ps + ws,        a|rgb2bgr(lerpXXX(color0, color1, unorm_sin(5.f * t)))),
        VertexPosColor(ps + justX(ws), a|rgb2bgr(lerpXXX(color0, color1, unorm_sin(7.f * t)))),
    };
    static const uint i[] = {0, 1, 2, 0, 2, 3};
    DrawElements(ShaderColorDither::instance(), ss, GL_TRIANGLES, v, i, arraySize(i));

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}    

void PushUnlockDial(TriMesh<VertexPosColor> &mesh, float2 pos, float rad, float progress,
                    uint color, float alpha)
{
    mesh.color(color, alpha * smooth_clamp(0.f, 1.f, progress, 0.2f));
    mesh.PushSector(pos, rad * lerp(1.3f, 1.f, progress), progress, M_PIf * progress);
}

static DEFINE_CVAR(float, kSpinnerRate, M_PI/2.f);

void renderLoadingSpinner(LineMesh<VertexPosColor> &mesh, float2 pos, float2 size, float alpha, float progress)
{
    const float ang = kSpinnerRate * globals.renderTime + M_TAOf * progress;
    const float2 rad = float2(min_dim(size) * 0.4f, 0.f);
    mesh.color(0xffffff, 0.5f * alpha);
    mesh.PushTri(pos + rotate(rad, ang),
                 pos + rotate(rad, ang+M_TAOf/3.f),
                 pos + rotate(rad, ang+2.f*M_TAOf/3.f));
}

void renderLoadingSpinner(ShaderState ss, float2 pos, float2 size, float alpha, float progress)
{
    const float ang = kSpinnerRate * globals.renderTime + M_TAOf * progress;
    const float2 rad = float2(min_dim(size) * 0.4f, 0.f);
    ss.color(0xffffff, 0.5f * alpha);
    ShaderUColor::instance().DrawLineTri(ss,
                                         pos + rotate(rad, ang),
                                         pos + rotate(rad, ang+M_TAOf/3.f),
                                         pos + rotate(rad, ang+2.f*M_TAOf/3.f));
}



void PostProc::BindWriteFramebuffer()
{
    getWrite().BindFramebuffer(m_res);
}

void PostProc::UnbindWriteFramebuffer()
{
    getWrite().UnbindFramebuffer();
}

void PostProc::Draw(bool bindFB)
{
    // assume write was just written
    const ShaderBlur *blurShader = m_blur ? &ShaderBlur::instance(m_blur) : NULL;

    if (m_blur)
    {
        glDisable(GL_BLEND);
        
        swapRW(); 
        BindWriteFramebuffer();
        blurShader->setDimension(1);
        getRead().DrawFullscreen(*blurShader);
        UnbindWriteFramebuffer();
        
        swapRW();
        BindWriteFramebuffer();
        blurShader->setDimension(0);
        getRead().DrawFullscreen(*blurShader);
        UnbindWriteFramebuffer();

        glEnable(GL_BLEND);
    }

    if (!bindFB)
    {
        getWrite().DrawFullscreen<ShaderTexture>();
    }
    // nothing to do if bindFB and no blur
}

View::View()
{
}


View operator+(const View& a, const View& b)
{
    View r(a);
    r.position = a.position + b.position;
    r.velocity = a.velocity + b.velocity;
    r.scale    = a.scale + b.scale;
    r.rot      = a.rot + b.rot;
    return r;
}

View operator*(float a, const View& b)
{
    View r(b);
    r.position = a * b.position;
    r.velocity = a * b.velocity;
    r.scale    = a * b.scale;
    r.rot      = a * b.rot;
    return r;
}

float View::getScale() const 
{
    return ((0.5f * sizePoints.y * scale) - z) / (0.5f * sizePoints.y); 
}

float2 View::toWorld(float2 p) const
{
    p -= 0.5f * sizePoints;
    p *= getScale();
    p = rotate(p, rot);
    p += position;
    return p;
}

float2 View::toScreen(float2 p) const
{
    p -= position;
    p = rotateN(p, rot);
    p /= getScale();
    p += 0.5f * sizePoints;
    return p;
}

bool View::intersectRectangle(const float3 &a, const float2 &r) const
{
    // FIXME take angle into account
    float2 zPlaneSize = (0.5f * scale * sizePoints) - getAspect() * a.z;
    return intersectRectangleRectangle(float2(a.x, a.y), r, 
                                       position, zPlaneSize);
}

void View::setScreenLineWidth(float scl) const
{
    const float width     = getScreenPointSizeInPixels();
    const float pointSize = sizePixels.x / sizePoints.x;
    const float lineWidth = clamp(width, 0.1f, 1.5f * pointSize);
    glLineWidth(lineWidth * scl);
    glReportError();
}

void View::setWorldLineWidth() const
{
    const float width     = getWorldPointSizeInPixels();
    const float pointSize = sizePixels.x / sizePoints.x;
    const float lineWidth = clamp(width, 0.1f, 1.5f * pointSize);
    glLineWidth(lineWidth);
    glReportError();
}

uint View::getCircleVerts(float worldRadius, int mx) const
{
    const uint verts = clamp(uint(round(toScreenSize(worldRadius))), 3, mx);
    return verts;
}

ShaderState View::getWorldShaderState(float2 zminmax) const
{
    static DEFINE_CVAR(float, kUpAngle, M_PI_2f);
    
    // +y is up in world coordinates
    const float2 s = 0.5f * sizePoints * float(scale);
    ShaderState ws;

#if 0
    ws.uTransform = glm::ortho(-s.x, s.x, -s.y, s.y, -10.f, 10.f);
    ws.rotate(-angle);
    ws.translate(-position);
#else
    const float fovy   = M_PI_2f;
    const float aspect = sizePoints.x / sizePoints.y;
    const float dist   = s.y;
    // const float mznear = min(1.f, dist - 100.f);
    const float mznear = clamp(dist + zminmax.x - 10.f, 1.f, dist - 10.f);
    const float mzfar  = dist + ((zminmax.y == 0.f) ? 2000.f : clamp(zminmax.y, 5.f, 10000.f));
    ASSERT(mznear < mzfar);

    const glm::mat4 view = glm::lookAt(float3(position, dist),
                                       float3(position, 0.f),
                                       float3(rotate(rot, kUpAngle), 0));
    const glm::mat4 proj = glm::perspective(fovy, aspect, mznear, mzfar);
    ws.uTransform = proj * view;
#endif

    ws.translateZ(z);

    return ws;
}

ShaderState View::getScreenShaderState() const
{
    ShaderState ss;
    static DEFINE_CVAR(float, kScreenFrustumDepth, 100.f);
    static DEFINE_CVAR(float, kMouseScreenSkew, -0.005f);

#if 0
    cpBB screenBB = getScreenPointBB();
    ss.uTransform = glm::ortho((float)screenBB.l, (float)screenBB.r, (float)screenBB.b, (float)screenBB.t, -10.f, 10.f);
#else
    const float2 offs = kMouseScreenSkew * (KeyState::instance().cursorPosScreen - 0.5f * globals.windowSizePoints);
    const float2 pos   = 0.5f * sizePoints;
    const float fovy   = M_PI_2f;
    const float aspect = sizePoints.x / sizePoints.y;
    const float dist   = pos.y;
    const float mznear  = max(dist - kScreenFrustumDepth, 1.f);

    const glm::mat4 view = glm::lookAt(float3(pos + offs, dist),
                                       float3(pos, 0.f),
                                       float3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(fovy, aspect, mznear, dist + kScreenFrustumDepth);
    ss.uTransform = proj * view;
#endif

    //ss.translate(float3(offset.x, offset.y, toScreenSize(offset.z)));
    //ss.translate(offset);

    return ss;
}


const GLTexture &getDitherTex()
{
    static GLTexture *tex = NULL;

    if (!tex)
    {
        static const char pattern[] = {
            0, 32,  8, 40,  2, 34, 10, 42,   /* 8x8 Bayer ordered dithering  */
            48, 16, 56, 24, 50, 18, 58, 26,   /* pattern.  Each input pixel   */
            12, 44,  4, 36, 14, 46,  6, 38,   /* is scaled to the 0..63 range */
            60, 28, 52, 20, 62, 30, 54, 22,   /* before looking in this table */
            3, 35, 11, 43,  1, 33,  9, 41,   /* to determine the action.     */
            51, 19, 59, 27, 49, 17, 57, 25,
            15, 47,  7, 39, 13, 45,  5, 37,
            63, 31, 55, 23, 61, 29, 53, 21 };

        GLuint name = 0;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 8, 8, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pattern);
        glReportError();

        tex = new GLTexture(name, float2(8.f), GL_LUMINANCE);
    }
    
    return *tex;
}
