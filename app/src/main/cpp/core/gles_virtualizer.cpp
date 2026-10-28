#include "gles_virtualizer.h"
#include "guest_memory.h"
#include "unicorn/arm.h"
#include <android/log.h>
#include <cmath>
#include <cstring>
#include <algorithm>

#define TAG "EmuxGlesVirt"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace emux {

// --- Matrix4 Implementation ---

Matrix4 Matrix4::identity() {
    return Matrix4();
}

Matrix4 Matrix4::operator*(const Matrix4& rhs) const {
    Matrix4 out;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            out.m[col * 4 + row] =
                m[0 * 4 + row] * rhs.m[col * 4 + 0] +
                m[1 * 4 + row] * rhs.m[col * 4 + 1] +
                m[2 * 4 + row] * rhs.m[col * 4 + 2] +
                m[3 * 4 + row] * rhs.m[col * 4 + 3];
        }
    }
    return out;
}

Matrix4 Matrix4::ortho(float left, float right, float bottom, float top, float zNear, float zFar) {
    Matrix4 res;
    float r_l = right - left;
    float t_b = top - bottom;
    float f_n = zFar - zNear;
    if (std::abs(r_l) < 1e-6f || std::abs(t_b) < 1e-6f || std::abs(f_n) < 1e-6f) {
        return res;
    }

    res.m[0] = 2.0f / r_l;
    res.m[5] = 2.0f / t_b;
    res.m[10] = -2.0f / f_n;
    res.m[12] = -(right + left) / r_l;
    res.m[13] = -(top + bottom) / t_b;
    res.m[14] = -(zFar + zNear) / f_n;
    res.m[15] = 1.0f;
    return res;
}

Matrix4 Matrix4::frustum(float left, float right, float bottom, float top, float zNear, float zFar) {
    Matrix4 res;
    float r_l = right - left;
    float t_b = top - bottom;
    float f_n = zFar - zNear;
    if (std::abs(r_l) < 1e-6f || std::abs(t_b) < 1e-6f || std::abs(f_n) < 1e-6f || zNear <= 0.0f || zFar <= 0.0f) {
        return res;
    }

    res.m[0] = (2.0f * zNear) / r_l;
    res.m[5] = (2.0f * zNear) / t_b;
    res.m[8] = (right + left) / r_l;
    res.m[9] = (top + bottom) / t_b;
    res.m[10] = -(zFar + zNear) / f_n;
    res.m[11] = -1.0f;
    res.m[14] = -(2.0f * zFar * zNear) / f_n;
    res.m[15] = 0.0f;
    return res;
}

Matrix4 Matrix4::translate(float x, float y, float z) {
    Matrix4 res = identity();
    res.m[12] = x;
    res.m[13] = y;
    res.m[14] = z;
    return res;
}

Matrix4 Matrix4::scale(float x, float y, float z) {
    Matrix4 res = identity();
    res.m[0] = x;
    res.m[5] = y;
    res.m[10] = z;
    return res;
}

Matrix4 Matrix4::rotate(float angleDeg, float x, float y, float z) {
    Matrix4 res = identity();
    float mag = std::sqrt(x * x + y * y + z * z);
    if (mag < 1e-6f) return res;
    x /= mag; y /= mag; z /= mag;

    float rad = angleDeg * 3.14159265358979323846f / 180.0f;
    float c = std::cos(rad);
    float s = std::sin(rad);
    float nc = 1.0f - c;

    res.m[0] = x * x * nc + c;
    res.m[1] = y * x * nc + z * s;
    res.m[2] = x * z * nc - y * s;

    res.m[4] = x * y * nc - z * s;
    res.m[5] = y * y * nc + c;
    res.m[6] = y * z * nc + x * s;

    res.m[8] = x * z * nc + y * s;
    res.m[9] = y * z * nc - x * s;
    res.m[10] = z * z * nc + c;

    return res;
}

// --- GlesVirtualizer Implementation ---

GlesVirtualizer& GlesVirtualizer::getInstance() {
    static GlesVirtualizer instance;
    return instance;
}

GlesVirtualizer::~GlesVirtualizer() {
    shutdown();
}

void GlesVirtualizer::initialize() {
    modelViewStack_.clear();
    modelViewStack_.push_back(Matrix4::identity());
    projectionStack_.clear();
    projectionStack_.push_back(Matrix4::identity());
    textureStack_.clear();
    textureStack_.push_back(Matrix4::identity());

    currentMatrixMode_ = GL_MODELVIEW;
    constantColor_ = {1.0f, 1.0f, 1.0f, 1.0f};
    texture2DEnabled_ = false;
    alphaTestEnabled_ = false;
    alphaFunc_ = GL_ALWAYS;
    alphaRef_ = 0.0f;
    clientActiveTexture_ = GL_TEXTURE0;
    shadeModel_ = GL_SMOOTH;

    for (size_t i = 0; i < kMaxVertexAttribs; ++i) {
        attribs_[i] = VertexAttribState();
    }
    shadowBuffersReady_ = false;
    ffProgramCompiled_ = false;
    LOGI("GlesVirtualizer inizializzato con successo");
}

void GlesVirtualizer::shutdown() {
    if (shadowBuffersReady_) {
        for (size_t i = 0; i < kMaxVertexAttribs; ++i) {
            if (shadowVbos_[i] != 0) {
                glDeleteBuffers(1, &shadowVbos_[i]);
                shadowVbos_[i] = 0;
            }
        }
        if (shadowEbo_ != 0) {
            glDeleteBuffers(1, &shadowEbo_);
            shadowEbo_ = 0;
        }
        shadowBuffersReady_ = false;
    }

    if (ffProgramCompiled_ && ffProgram_ != 0) {
        glDeleteProgram(ffProgram_);
        ffProgram_ = 0;
        ffProgramCompiled_ = false;
    }
}

void GlesVirtualizer::ensureInitialized() {
    if (!shadowBuffersReady_) {
        glGenBuffers(static_cast<GLsizei>(kMaxVertexAttribs), shadowVbos_);
        glGenBuffers(1, &shadowEbo_);
        shadowBuffersReady_ = true;
        LOGI("GlesVirtualizer: 16 Shadow VBOs + 1 Shadow EBO allocati su GPU");
    }
}

std::vector<Matrix4>& GlesVirtualizer::currentMatrixStack() {
    switch (currentMatrixMode_) {
        case GL_PROJECTION: return projectionStack_;
        case GL_TEXTURE:    return textureStack_;
        case GL_MODELVIEW:
        default:            return modelViewStack_;
    }
}

void GlesVirtualizer::notifyBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) {
        currentArrayBuffer_ = buffer;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        currentElementArrayBuffer_ = buffer;
    }
}

void GlesVirtualizer::enableVertexAttribArray(GLuint index) {
    if (index < kMaxVertexAttribs) {
        attribs_[index].enabled = true;
    }
}

void GlesVirtualizer::disableVertexAttribArray(GLuint index) {
    if (index < kMaxVertexAttribs) {
        attribs_[index].enabled = false;
    }
}

void GlesVirtualizer::setVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, uint32_t ptr) {
    if (index >= kMaxVertexAttribs) return;

    attribs_[index].size = size;
    attribs_[index].type = type;
    attribs_[index].normalized = normalized;
    attribs_[index].stride = stride;
    attribs_[index].guestPtr = ptr;
    attribs_[index].boundVbo = currentArrayBuffer_;

    if (currentArrayBuffer_ != 0) {
        glVertexAttribPointer(index, size, type, normalized, stride, reinterpret_cast<const void*>(static_cast<uintptr_t>(ptr)));
    }
}

static inline size_t getGlTypeSize(GLenum type) {
    switch (type) {
        case GL_BYTE:
        case GL_UNSIGNED_BYTE:  return 1;
        case GL_SHORT:
        case GL_UNSIGNED_SHORT: return 2;
        case GL_FLOAT:
        case GL_FIXED:          return 4;
        default:                return 4;
    }
}

void GlesVirtualizer::syncClientVertexAttributes(GLint first, GLsizei count, uc_engine* uc) {
    if (count <= 0) return;
    auto& gm = GuestMemory::getSharedInstance();

    for (GLuint i = 0; i < kMaxVertexAttribs; ++i) {
        if (!attribs_[i].enabled) continue;
        if (attribs_[i].boundVbo != 0) continue; // Già su VBO guest valido
        if (attribs_[i].guestPtr == 0) continue;

        size_t typeSz = getGlTypeSize(attribs_[i].type);
        size_t elemSz = static_cast<size_t>(attribs_[i].size) * typeSz;
        size_t effStride = (attribs_[i].stride == 0) ? elemSz : static_cast<size_t>(attribs_[i].stride);

        size_t totalBytes = static_cast<size_t>(first + count) * effStride;
        if (totalBytes == 0 || totalBytes > 16 * 1024 * 1024) continue; // Sanity check max 16MB

        const void* hostSrc = nullptr;
        std::vector<uint8_t> readFallback;
        if (gm.isReady() && gm.isMappedRange(attribs_[i].guestPtr, totalBytes)) {
            hostSrc = gm.g2h(attribs_[i].guestPtr);
        } else if (uc) {
            readFallback.resize(totalBytes);
            if (uc_mem_read(uc, attribs_[i].guestPtr, readFallback.data(), totalBytes) == UC_ERR_OK) {
                hostSrc = readFallback.data();
            }
        }

        if (hostSrc) {
            glBindBuffer(GL_ARRAY_BUFFER, shadowVbos_[i]);
            glBufferData(GL_ARRAY_BUFFER, totalBytes, hostSrc, GL_DYNAMIC_DRAW);
            glVertexAttribPointer(i, attribs_[i].size, attribs_[i].type, attribs_[i].normalized,
                                  attribs_[i].stride, reinterpret_cast<const void*>(0));
        }
    }
}

const void* GlesVirtualizer::uploadShadowIndices(GLsizei count, GLenum type, uint32_t indicesPtr, uc_engine* uc, uint32_t& outMaxIndex) {
    outMaxIndex = 0;
    if (count <= 0 || indicesPtr == 0) return nullptr;

    size_t idxSize = (type == GL_UNSIGNED_SHORT) ? 2 : ((type == GL_UNSIGNED_INT) ? 4 : 1);
    size_t totalBytes = static_cast<size_t>(count) * idxSize;
    if (totalBytes > 8 * 1024 * 1024) return nullptr;

    auto& gm = GuestMemory::getSharedInstance();
    const void* hostSrc = nullptr;
    std::vector<uint8_t> readFallback;

    if (gm.isReady() && gm.isMappedRange(indicesPtr, totalBytes)) {
        hostSrc = gm.g2h(indicesPtr);
    } else if (uc) {
        readFallback.resize(totalBytes);
        if (uc_mem_read(uc, indicesPtr, readFallback.data(), totalBytes) == UC_ERR_OK) {
            hostSrc = readFallback.data();
        }
    }

    if (!hostSrc) return nullptr;

    // Scansione indice massimo per caricare tutti i vertici necessari negli shadow VBO
    if (type == GL_UNSIGNED_SHORT) {
        const auto* u16 = reinterpret_cast<const uint16_t*>(hostSrc);
        for (GLsizei k = 0; k < count; ++k) {
            if (u16[k] > outMaxIndex) outMaxIndex = u16[k];
        }
    } else if (type == GL_UNSIGNED_BYTE) {
        const auto* u8 = reinterpret_cast<const uint8_t*>(hostSrc);
        for (GLsizei k = 0; k < count; ++k) {
            if (u8[k] > outMaxIndex) outMaxIndex = u8[k];
        }
    } else if (type == GL_UNSIGNED_INT) {
        const auto* u32 = reinterpret_cast<const uint32_t*>(hostSrc);
        for (GLsizei k = 0; k < count; ++k) {
            if (u32[k] > outMaxIndex) outMaxIndex = u32[k];
        }
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shadowEbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, totalBytes, hostSrc, GL_DYNAMIC_DRAW);
    return reinterpret_cast<const void*>(0);
}

void GlesVirtualizer::restoreVertexAttributes() {
    glBindBuffer(GL_ARRAY_BUFFER, currentArrayBuffer_);
}

// --- Shader Fixed-Function Pipeline Emulation ---

bool GlesVirtualizer::buildFixedFunctionShaders() {
    if (ffProgramCompiled_) return true;

    const char* vsSrc =
        "attribute vec4 a_position;\n"
        "attribute vec2 a_texCoord;\n"
        "attribute vec4 a_color;\n"
        "uniform mat4 u_mvp;\n"
        "uniform int u_useColorArray;\n"
        "uniform vec4 u_constantColor;\n"
        "varying vec4 v_color;\n"
        "varying vec2 v_texCoord;\n"
        "void main() {\n"
        "    gl_Position = u_mvp * a_position;\n"
        "    v_color = (u_useColorArray != 0) ? a_color : u_constantColor;\n"
        "    v_texCoord = a_texCoord;\n"
        "}\n";

    const char* fsSrc =
        "precision mediump float;\n"
        "varying vec4 v_color;\n"
        "varying vec2 v_texCoord;\n"
        "uniform sampler2D u_texture;\n"
        "uniform int u_useTexture;\n"
        "uniform int u_alphaTest;\n"
        "uniform float u_alphaRef;\n"
        "void main() {\n"
        "    vec4 col = v_color;\n"
        "    if (u_useTexture != 0) {\n"
        "        col = col * texture2D(u_texture, v_texCoord);\n"
        "    }\n"
        "    if (u_alphaTest != 0 && col.a < u_alphaRef) {\n"
        "        discard;\n"
        "    }\n"
        "    gl_FragColor = col;\n"
        "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSrc, nullptr);
    glCompileShader(vs);

    GLint compiled = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        LOGE("GlesVirtualizer: Errore compilazione Vertex Shader Fixed-Function");
        glDeleteShader(vs);
        return false;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSrc, nullptr);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        LOGE("GlesVirtualizer: Errore compilazione Fragment Shader Fixed-Function");
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    ffProgram_ = glCreateProgram();
    glAttachShader(ffProgram_, vs);
    glAttachShader(ffProgram_, fs);

    // Mappatura esplicita attributi corrispondenti alle chiamate fixed-function
    glBindAttribLocation(ffProgram_, 0, "a_position"); // glVertexPointer
    glBindAttribLocation(ffProgram_, 1, "a_texCoord"); // glTexCoordPointer
    glBindAttribLocation(ffProgram_, 2, "a_color");    // glColorPointer

    glLinkProgram(ffProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(ffProgram_, GL_LINK_STATUS, &linked);
    if (!linked) {
        LOGE("GlesVirtualizer: Errore linking Shader Program Fixed-Function");
        glDeleteProgram(ffProgram_);
        ffProgram_ = 0;
        return false;
    }

    uMvpLoc_           = glGetUniformLocation(ffProgram_, "u_mvp");
    uUseColorArrayLoc_ = glGetUniformLocation(ffProgram_, "u_useColorArray");
    uConstantColorLoc_ = glGetUniformLocation(ffProgram_, "u_constantColor");
    uUseTextureLoc_    = glGetUniformLocation(ffProgram_, "u_useTexture");
    uTextureLoc_       = glGetUniformLocation(ffProgram_, "u_texture");
    uAlphaTestLoc_     = glGetUniformLocation(ffProgram_, "u_alphaTest");
    uAlphaRefLoc_      = glGetUniformLocation(ffProgram_, "u_alphaRef");

    ffProgramCompiled_ = true;
    LOGI("GlesVirtualizer: Shader Program Fixed-Function GLES 1.1 linkato con successo");
    return true;
}

void GlesVirtualizer::setupFixedFunctionPipeline() {
    if (!buildFixedFunctionShaders()) return;

    glUseProgram(ffProgram_);

    // Calcolo matrice MVP = Projection * ModelView
    Matrix4 mv = modelViewStack_.empty() ? Matrix4::identity() : modelViewStack_.back();
    Matrix4 pr = projectionStack_.empty() ? Matrix4::identity() : projectionStack_.back();
    Matrix4 mvp = pr * mv;

    if (uMvpLoc_ >= 0) {
        glUniformMatrix4fv(uMvpLoc_, 1, GL_FALSE, mvp.m);
    }
    if (uUseColorArrayLoc_ >= 0) {
        glUniform1i(uUseColorArrayLoc_, attribs_[2].enabled ? 1 : 0);
    }
    if (uConstantColorLoc_ >= 0) {
        glUniform4f(uConstantColorLoc_, constantColor_.r, constantColor_.g, constantColor_.b, constantColor_.a);
    }
    if (uUseTextureLoc_ >= 0) {
        glUniform1i(uUseTextureLoc_, texture2DEnabled_ ? 1 : 0);
    }
    if (uTextureLoc_ >= 0) {
        glUniform1i(uTextureLoc_, 0); // Texture Unit 0
    }
    if (uAlphaTestLoc_ >= 0) {
        glUniform1i(uAlphaTestLoc_, alphaTestEnabled_ ? 1 : 0);
    }
    if (uAlphaRefLoc_ >= 0) {
        glUniform1f(uAlphaRefLoc_, alphaRef_);
    }
}

void GlesVirtualizer::drawArrays(GLenum mode, GLint first, GLsizei count, uc_engine* uc) {
    if (count <= 0) return;
    ensureInitialized();

    GLint curProg = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &curProg);
    if (curProg == 0) {
        setupFixedFunctionPipeline();
    }

    syncClientVertexAttributes(first, count, uc);
    glDrawArrays(mode, first, count);
    restoreVertexAttributes();
}

void GlesVirtualizer::drawElements(GLenum mode, GLsizei count, GLenum type, uint32_t indicesPtr, uc_engine* uc) {
    if (count <= 0) return;
    ensureInitialized();

    GLint curProg = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &curProg);
    if (curProg == 0) {
        setupFixedFunctionPipeline();
    }

    uint32_t maxIndex = 0;
    bool hasClientIndices = (currentElementArrayBuffer_ == 0 && indicesPtr != 0);
    const void* drawIndices = reinterpret_cast<const void*>(static_cast<uintptr_t>(indicesPtr));

    if (hasClientIndices) {
        drawIndices = uploadShadowIndices(count, type, indicesPtr, uc, maxIndex);
    } else {
        maxIndex = static_cast<uint32_t>(count);
    }

    syncClientVertexAttributes(0, static_cast<GLsizei>(maxIndex + 1), uc);
    glDrawElements(mode, count, type, drawIndices);
    restoreVertexAttributes();

    if (hasClientIndices) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, currentElementArrayBuffer_);
    }
}

// --- Dispatcher per Simboli OpenGL ES 1.1 Fixed-Function ---

static inline float readFloat(uint32_t raw) {
    float f = 0.0f;
    std::memcpy(&f, &raw, 4);
    return f;
}

bool GlesVirtualizer::handleGles1(const std::string& name, uc_engine* uc) {
    if (name == "glEnableClientState") {
        uint32_t cap = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cap);
        if (cap == GL_VERTEX_ARRAY) enableVertexAttribArray(0);
        else if (cap == GL_TEXTURE_COORD_ARRAY) enableVertexAttribArray(1);
        else if (cap == GL_COLOR_ARRAY) enableVertexAttribArray(2);
        else if (cap == GL_NORMAL_ARRAY) enableVertexAttribArray(3);
        glEnableVertexAttribArray(cap == GL_VERTEX_ARRAY ? 0 :
                                 (cap == GL_TEXTURE_COORD_ARRAY ? 1 :
                                 (cap == GL_COLOR_ARRAY ? 2 : 3)));
        return true;
    }
    if (name == "glDisableClientState") {
        uint32_t cap = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &cap);
        if (cap == GL_VERTEX_ARRAY) disableVertexAttribArray(0);
        else if (cap == GL_TEXTURE_COORD_ARRAY) disableVertexAttribArray(1);
        else if (cap == GL_COLOR_ARRAY) disableVertexAttribArray(2);
        else if (cap == GL_NORMAL_ARRAY) disableVertexAttribArray(3);
        glDisableVertexAttribArray(cap == GL_VERTEX_ARRAY ? 0 :
                                  (cap == GL_TEXTURE_COORD_ARRAY ? 1 :
                                  (cap == GL_COLOR_ARRAY ? 2 : 3)));
        return true;
    }
    if (name == "glVertexPointer") {
        uint32_t size = 0, type = 0, stride = 0, ptr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &size);
        uc_reg_read(uc, UC_ARM_REG_R1, &type);
        uc_reg_read(uc, UC_ARM_REG_R2, &stride);
        uc_reg_read(uc, UC_ARM_REG_R3, &ptr);
        setVertexAttribPointer(0, static_cast<GLint>(size), type, GL_FALSE, static_cast<GLsizei>(stride), ptr);
        return true;
    }
    if (name == "glTexCoordPointer") {
        uint32_t size = 0, type = 0, stride = 0, ptr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &size);
        uc_reg_read(uc, UC_ARM_REG_R1, &type);
        uc_reg_read(uc, UC_ARM_REG_R2, &stride);
        uc_reg_read(uc, UC_ARM_REG_R3, &ptr);
        setVertexAttribPointer(1, static_cast<GLint>(size), type, GL_FALSE, static_cast<GLsizei>(stride), ptr);
        return true;
    }
    if (name == "glColorPointer") {
        uint32_t size = 0, type = 0, stride = 0, ptr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &size);
        uc_reg_read(uc, UC_ARM_REG_R1, &type);
        uc_reg_read(uc, UC_ARM_REG_R2, &stride);
        uc_reg_read(uc, UC_ARM_REG_R3, &ptr);
        GLboolean norm = (type == GL_UNSIGNED_BYTE) ? GL_TRUE : GL_FALSE;
        setVertexAttribPointer(2, static_cast<GLint>(size), type, norm, static_cast<GLsizei>(stride), ptr);
        return true;
    }
    if (name == "glNormalPointer") {
        uint32_t type = 0, stride = 0, ptr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &type);
        uc_reg_read(uc, UC_ARM_REG_R1, &stride);
        uc_reg_read(uc, UC_ARM_REG_R2, &ptr);
        setVertexAttribPointer(3, 3, type, GL_FALSE, static_cast<GLsizei>(stride), ptr);
        return true;
    }
    if (name == "glPointSizePointerOES") {
        return true;
    }
    if (name == "glMatrixMode") {
        uint32_t mode = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mode);
        currentMatrixMode_ = mode;
        return true;
    }
    if (name == "glLoadIdentity") {
        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = Matrix4::identity();
        }
        return true;
    }
    if (name == "glLoadMatrixf") {
        uint32_t mPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mPtr);
        if (mPtr != 0) {
            float raw[16];
            uc_mem_read(uc, mPtr, raw, sizeof(raw));
            auto& stack = currentMatrixStack();
            if (!stack.empty()) {
                std::memcpy(stack.back().m, raw, sizeof(raw));
            }
        }
        return true;
    }
    if (name == "glLoadMatrixx") {
        uint32_t mPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mPtr);
        if (mPtr != 0) {
            int32_t fixed[16];
            uc_mem_read(uc, mPtr, fixed, sizeof(fixed));
            auto& stack = currentMatrixStack();
            if (!stack.empty()) {
                for (int i = 0; i < 16; ++i) {
                    stack.back().m[i] = static_cast<float>(fixed[i]) / 65536.0f;
                }
            }
        }
        return true;
    }
    if (name == "glMultMatrixf") {
        uint32_t mPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mPtr);
        if (mPtr != 0) {
            Matrix4 rhs;
            uc_mem_read(uc, mPtr, rhs.m, sizeof(rhs.m));
            auto& stack = currentMatrixStack();
            if (!stack.empty()) {
                stack.back() = stack.back() * rhs;
            }
        }
        return true;
    }
    if (name == "glMultMatrixx") {
        uint32_t mPtr = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &mPtr);
        if (mPtr != 0) {
            int32_t fixed[16];
            uc_mem_read(uc, mPtr, fixed, sizeof(fixed));
            Matrix4 rhs;
            for (int i = 0; i < 16; ++i) {
                rhs.m[i] = static_cast<float>(fixed[i]) / 65536.0f;
            }
            auto& stack = currentMatrixStack();
            if (!stack.empty()) {
                stack.back() = stack.back() * rhs;
            }
        }
        return true;
    }
    if (name == "glPushMatrix") {
        auto& stack = currentMatrixStack();
        if (!stack.empty() && stack.size() < 32) {
            stack.push_back(stack.back());
        }
        return true;
    }
    if (name == "glPopMatrix") {
        auto& stack = currentMatrixStack();
        if (stack.size() > 1) {
            stack.pop_back();
        }
        return true;
    }
    if (name == "glOrthof") {
        uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0, nRaw = 0, fRaw = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp + 0, &nRaw, 4);
        uc_mem_read(uc, sp + 4, &fRaw, 4);

        float l = readFloat(r0), r = readFloat(r1), b = readFloat(r2), t = readFloat(r3);
        float n = readFloat(nRaw), f = readFloat(fRaw);

        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::ortho(l, r, b, t, n, f);
        }
        return true;
    }
    if (name == "glOrthox") {
        int32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        uint32_t sp = 0;
        int32_t nFixed = 0, fFixed = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp + 0, &nFixed, 4);
        uc_mem_read(uc, sp + 4, &fFixed, 4);

        float l = r0 / 65536.0f, r = r1 / 65536.0f, b = r2 / 65536.0f, t = r3 / 65536.0f;
        float n = nFixed / 65536.0f, f = fFixed / 65536.0f;

        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::ortho(l, r, b, t, n, f);
        }
        return true;
    }
    if (name == "glFrustumf") {
        uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, sp = 0, nRaw = 0, fRaw = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp + 0, &nRaw, 4);
        uc_mem_read(uc, sp + 4, &fRaw, 4);

        float l = readFloat(r0), r = readFloat(r1), b = readFloat(r2), t = readFloat(r3);
        float n = readFloat(nRaw), f = readFloat(fRaw);

        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::frustum(l, r, b, t, n, f);
        }
        return true;
    }
    if (name == "glFrustumx") {
        int32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        uint32_t sp = 0;
        int32_t nFixed = 0, fFixed = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        uc_reg_read(uc, UC_ARM_REG_SP, &sp);
        uc_mem_read(uc, sp + 0, &nFixed, 4);
        uc_mem_read(uc, sp + 4, &fFixed, 4);

        float l = r0 / 65536.0f, r = r1 / 65536.0f, b = r2 / 65536.0f, t = r3 / 65536.0f;
        float n = nFixed / 65536.0f, f = fFixed / 65536.0f;

        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::frustum(l, r, b, t, n, f);
        }
        return true;
    }
    if (name == "glTranslatef") {
        uint32_t r0 = 0, r1 = 0, r2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        float x = readFloat(r0), y = readFloat(r1), z = readFloat(r2);
        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::translate(x, y, z);
        }
        return true;
    }
    if (name == "glTranslatex") {
        int32_t r0 = 0, r1 = 0, r2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        float x = r0 / 65536.0f, y = r1 / 65536.0f, z = r2 / 65536.0f;
        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::translate(x, y, z);
        }
        return true;
    }
    if (name == "glRotatef") {
        uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        float a = readFloat(r0), x = readFloat(r1), y = readFloat(r2), z = readFloat(r3);
        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::rotate(a, x, y, z);
        }
        return true;
    }
    if (name == "glRotatex") {
        int32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        float a = r0 / 65536.0f, x = r1 / 65536.0f, y = r2 / 65536.0f, z = r3 / 65536.0f;
        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::rotate(a, x, y, z);
        }
        return true;
    }
    if (name == "glScalef") {
        uint32_t r0 = 0, r1 = 0, r2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        float x = readFloat(r0), y = readFloat(r1), z = readFloat(r2);
        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::scale(x, y, z);
        }
        return true;
    }
    if (name == "glScalex") {
        int32_t r0 = 0, r1 = 0, r2 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        float x = r0 / 65536.0f, y = r1 / 65536.0f, z = r2 / 65536.0f;
        auto& stack = currentMatrixStack();
        if (!stack.empty()) {
            stack.back() = stack.back() * Matrix4::scale(x, y, z);
        }
        return true;
    }
    if (name == "glColor4f") {
        uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        constantColor_ = {readFloat(r0), readFloat(r1), readFloat(r2), readFloat(r3)};
        return true;
    }
    if (name == "glColor4ub") {
        uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        constantColor_ = { (r0 & 0xFF) / 255.0f, (r1 & 0xFF) / 255.0f, (r2 & 0xFF) / 255.0f, (r3 & 0xFF) / 255.0f };
        return true;
    }
    if (name == "glColor4x") {
        int32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &r0);
        uc_reg_read(uc, UC_ARM_REG_R1, &r1);
        uc_reg_read(uc, UC_ARM_REG_R2, &r2);
        uc_reg_read(uc, UC_ARM_REG_R3, &r3);
        constantColor_ = { r0 / 65536.0f, r1 / 65536.0f, r2 / 65536.0f, r3 / 65536.0f };
        return true;
    }
    if (name == "glAlphaFunc") {
        uint32_t func = 0, rawRef = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &func);
        uc_reg_read(uc, UC_ARM_REG_R1, &rawRef);
        alphaFunc_ = func;
        alphaRef_ = readFloat(rawRef);
        return true;
    }
    if (name == "glAlphaFuncx") {
        uint32_t func = 0;
        int32_t refFixed = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &func);
        uc_reg_read(uc, UC_ARM_REG_R1, &refFixed);
        alphaFunc_ = func;
        alphaRef_ = refFixed / 65536.0f;
        return true;
    }
    if (name == "glTexEnvf" || name == "glTexEnvi" || name == "glTexEnvx") {
        return true;
    }
    if (name == "glClientActiveTexture") {
        uint32_t tex = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &tex);
        clientActiveTexture_ = tex;
        return true;
    }
    if (name == "glShadeModel") {
        uint32_t sm = 0;
        uc_reg_read(uc, UC_ARM_REG_R0, &sm);
        shadeModel_ = sm;
        return true;
    }
    if (name == "glNormal3f" || name == "glNormal3x") {
        return true;
    }
    if (name == "glPointSize" || name == "glPointSizex") {
        return true;
    }
    if (name.rfind("glLight", 0) == 0 || name.rfind("glMaterial", 0) == 0 || name.rfind("glFog", 0) == 0) {
        return true;
    }

    return false;
}

bool GlesVirtualizer::handleEnableDisable(const std::string& name, GLenum cap) {
    bool enable = (name == "glEnable");
    if (cap == 0x0DE1 /* GL_TEXTURE_2D */) {
        texture2DEnabled_ = enable;
        return true; // Gestito internamente nello shim shader
    }
    if (cap == GL_ALPHA_TEST) {
        alphaTestEnabled_ = enable;
        return true; // Gestito internamente nello shim shader
    }
    if (cap == GL_LIGHTING || cap == GL_FOG || cap == GL_COLOR_MATERIAL ||
        cap == GL_POINT_SMOOTH || cap == GL_LINE_SMOOTH) {
        return true; // Ignora enum GLES 1.1 non supportati nel core profile GLES 2.0/3.0
    }
    return false; // Lascia procedere verso host glEnable/glDisable
}

bool GlesVirtualizer::handleIsEnabled(GLenum cap, uint32_t& outResult) {
    if (cap == 0x0DE1 /* GL_TEXTURE_2D */) {
        outResult = texture2DEnabled_ ? 1U : 0U;
        return true;
    }
    if (cap == GL_ALPHA_TEST) {
        outResult = alphaTestEnabled_ ? 1U : 0U;
        return true;
    }
    return false;
}

} // namespace emux
