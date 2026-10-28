#pragma once

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "unicorn/unicorn.h"

// Costanti standard OpenGL ES 1.1 non presenti in GLES2/gl2.h
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY              0x8074
#endif
#ifndef GL_NORMAL_ARRAY
#define GL_NORMAL_ARRAY              0x8075
#endif
#ifndef GL_COLOR_ARRAY
#define GL_COLOR_ARRAY               0x8076
#endif
#ifndef GL_TEXTURE_COORD_ARRAY
#define GL_TEXTURE_COORD_ARRAY       0x8078
#endif
#ifndef GL_MODELVIEW
#define GL_MODELVIEW                 0x1700
#endif
#ifndef GL_PROJECTION
#define GL_PROJECTION                0x1701
#endif
#ifndef GL_TEXTURE
#define GL_TEXTURE                   0x1702
#endif
#ifndef GL_ALPHA_TEST
#define GL_ALPHA_TEST                0x0BC0
#endif
#ifndef GL_LIGHTING
#define GL_LIGHTING                  0x0B50
#endif
#ifndef GL_FOG
#define GL_FOG                       0x0B60
#endif
#ifndef GL_COLOR_MATERIAL
#define GL_COLOR_MATERIAL            0x0B57
#endif
#ifndef GL_TEXTURE_ENV
#define GL_TEXTURE_ENV               0x2300
#endif
#ifndef GL_TEXTURE_ENV_MODE
#define GL_TEXTURE_ENV_MODE          0x2200
#endif
#ifndef GL_MODULATE
#define GL_MODULATE                  0x2100
#endif
#ifndef GL_DECAL
#define GL_DECAL                     0x2101
#endif
#ifndef GL_POINT_SMOOTH
#define GL_POINT_SMOOTH              0x0B10
#endif
#ifndef GL_LINE_SMOOTH
#define GL_LINE_SMOOTH               0x0B20
#endif
#ifndef GL_PERSPECTIVE_CORRECTION_HINT
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#endif
#ifndef GL_POINT_SIZE_ARRAY_OES
#define GL_POINT_SIZE_ARRAY_OES      0x8B9C
#endif
#ifndef GL_FLAT
#define GL_FLAT                      0x1D00
#endif
#ifndef GL_SMOOTH
#define GL_SMOOTH                    0x1D01
#endif

namespace emux {

struct Matrix4 {
    float m[16]{1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f};

    static Matrix4 identity();
    Matrix4 operator*(const Matrix4& rhs) const;

    static Matrix4 ortho(float left, float right, float bottom, float top, float zNear, float zFar);
    static Matrix4 frustum(float left, float right, float bottom, float top, float zNear, float zFar);
    static Matrix4 translate(float x, float y, float z);
    static Matrix4 scale(float x, float y, float z);
    static Matrix4 rotate(float angleDeg, float x, float y, float z);
};

struct VertexAttribState {
    bool enabled{false};
    GLint size{4};
    GLenum type{GL_FLOAT};
    GLboolean normalized{GL_FALSE};
    GLsizei stride{0};
    uint32_t guestPtr{0};
    GLuint boundVbo{0};
};

class GlesVirtualizer {
public:
    static GlesVirtualizer& getInstance();

    void initialize();
    void shutdown();

    void notifyBindBuffer(GLenum target, GLuint buffer);
    void enableVertexAttribArray(GLuint index);
    void disableVertexAttribArray(GLuint index);
    void setVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, uint32_t ptr);

    void drawArrays(GLenum mode, GLint first, GLsizei count, uc_engine* uc);
    void drawElements(GLenum mode, GLsizei count, GLenum type, uint32_t indicesPtr, uc_engine* uc);

    bool handleGles1(const std::string& name, uc_engine* uc);
    bool handleEnableDisable(const std::string& name, GLenum cap);
    bool handleIsEnabled(GLenum cap, uint32_t& outResult);

    GLuint getCurrentArrayBuffer() const { return currentArrayBuffer_; }
    GLuint getCurrentElementArrayBuffer() const { return currentElementArrayBuffer_; }

private:
    GlesVirtualizer() = default;
    ~GlesVirtualizer();

    void ensureInitialized();
    void syncClientVertexAttributes(GLint first, GLsizei count, uc_engine* uc);
    const void* uploadShadowIndices(GLsizei count, GLenum type, uint32_t indicesPtr, uc_engine* uc, uint32_t& outMaxIndex);
    void restoreVertexAttributes();

    void setupFixedFunctionPipeline();
    bool buildFixedFunctionShaders();

    std::vector<Matrix4>& currentMatrixStack();

    // Stato Vertex Attributes
    static constexpr size_t kMaxVertexAttribs = 16;
    VertexAttribState attribs_[kMaxVertexAttribs];
    GLuint shadowVbos_[kMaxVertexAttribs]{0};
    GLuint shadowEbo_{0};
    bool shadowBuffersReady_{false};

    GLuint currentArrayBuffer_{0};
    GLuint currentElementArrayBuffer_{0};

    // Stacks Matrici Fixed-Function
    GLenum currentMatrixMode_{GL_MODELVIEW};
    std::vector<Matrix4> modelViewStack_;
    std::vector<Matrix4> projectionStack_;
    std::vector<Matrix4> textureStack_;

    // Stato Fixed-Function
    struct {
        float r{1.0f}, g{1.0f}, b{1.0f}, a{1.0f};
    } constantColor_;

    bool texture2DEnabled_{false};
    bool alphaTestEnabled_{false};
    GLenum alphaFunc_{GL_ALWAYS};
    float alphaRef_{0.0f};
    GLenum clientActiveTexture_{GL_TEXTURE0};
    GLenum shadeModel_{GL_SMOOTH};

    // Shader Program per Emulazione Fixed-Function
    GLuint ffProgram_{0};
    GLint uMvpLoc_{-1};
    GLint uUseColorArrayLoc_{-1};
    GLint uConstantColorLoc_{-1};
    GLint uUseTextureLoc_{-1};
    GLint uTextureLoc_{-1};
    GLint uAlphaTestLoc_{-1};
    GLint uAlphaRefLoc_{-1};
    bool ffProgramCompiled_{false};
};

} // namespace emux
