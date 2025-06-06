#include "Texture.hpp"
#include "Renderer.hpp"
#include "../Compositor.hpp"
#include "../protocols/types/Buffer.hpp"
#include "../helpers/Format.hpp"
#include <cstring>

CTexture::CTexture() = default;

CTexture::~CTexture() {
    if (!g_pCompositor || g_pCompositor->m_isShuttingDown || !g_pHyprRenderer)
        return;

    g_pHyprRenderer->makeEGLCurrent();
    destroyTexture();
}

CTexture::CTexture(uint32_t drmFormat, uint8_t* pixels, uint32_t stride, const Vector2D& size_, bool keepDataCopy) : m_drmFormat(drmFormat), m_keepDataCopy(keepDataCopy) {
    createFromShm(drmFormat, pixels, stride, size_);
}

CTexture::CTexture(const Aquamarine::SDMABUFAttrs& attrs, void* image) {
    createFromDma(attrs, image);
}

CTexture::CTexture(const SP<Aquamarine::IBuffer> buffer, bool keepDataCopy) : m_keepDataCopy(keepDataCopy) {
    if (!buffer)
        return;

    m_opaque = buffer->opaque;

    auto attrs = buffer->dmabuf();

    if (!attrs.success) {
        // attempt shm
        auto shm = buffer->shm();

        if (!shm.success) {
            Debug::log(ERR, "Cannot create a texture: buffer has no dmabuf or shm");
            return;
        }

        auto [pixelData, fmt, bufLen] = buffer->beginDataPtr(0);

        m_drmFormat = fmt;

        createFromShm(fmt, pixelData, bufLen, shm.size);
        return;
    }

    auto image = g_pHyprOpenGL->createEGLImage(buffer->dmabuf());

    if (!image) {
        Debug::log(ERR, "Cannot create a texture: failed to create an EGLImage");
        return;
    }

    createFromDma(attrs, image);
}

void CTexture::createFromShm(uint32_t drmFormat, uint8_t* pixels, uint32_t stride, const Vector2D& size_) {
    g_pHyprRenderer->makeEGLCurrent();

    const auto format = NFormatUtils::getPixelFormatFromDRM(drmFormat);
    ASSERT(format);

    m_type          = format->withAlpha ? TEXTURE_RGBA : TEXTURE_RGBX;
    m_size          = size_;
    m_isSynchronous = true;
    allocate();

    GLCALL(glBindTexture(GL_TEXTURE_2D, m_texID));
    GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
#ifndef GLES2
    if (format->flipRB) {
        GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE));
        GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED));
    }
#endif
    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / format->bytesPerBlock));
    GLCALL(glTexImage2D(GL_TEXTURE_2D, 0, format->glInternalFormat ? format->glInternalFormat : format->glFormat, size_.x, size_.y, 0, format->glFormat, format->glType, pixels));
    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0));
    GLCALL(glBindTexture(GL_TEXTURE_2D, 0));

    if (m_keepDataCopy) {
        m_dataCopy.resize(stride * size_.y);
        memcpy(m_dataCopy.data(), pixels, stride * size_.y);
    }
}

void CTexture::createFromDma(const Aquamarine::SDMABUFAttrs& attrs, void* image) {
    if (!g_pHyprOpenGL->m_proc.glEGLImageTargetTexture2DOES) {
        Debug::log(ERR, "Cannot create a dmabuf texture: no glEGLImageTargetTexture2DOES");
        return;
    }

    m_opaque = NFormatUtils::isFormatOpaque(attrs.format);
    m_target = GL_TEXTURE_2D;
    m_type   = TEXTURE_RGBA;
    m_size   = attrs.size;
    m_type   = NFormatUtils::isFormatOpaque(attrs.format) ? TEXTURE_RGBX : TEXTURE_RGBA;
    allocate();
    m_eglImage = image;

    GLCALL(glBindTexture(GL_TEXTURE_2D, m_texID));
    GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GLCALL(g_pHyprOpenGL->m_proc.glEGLImageTargetTexture2DOES(m_target, image));
    GLCALL(glBindTexture(GL_TEXTURE_2D, 0));
}

void CTexture::update(uint32_t drmFormat, uint8_t* pixels, uint32_t stride, const CRegion& damage) {
    g_pHyprRenderer->makeEGLCurrent();

    const auto format = NFormatUtils::getPixelFormatFromDRM(drmFormat);
    ASSERT(format);

    glBindTexture(GL_TEXTURE_2D, m_texID);

    auto rects = damage.copy().intersect(CBox{{}, m_size}).getRects();

#ifndef GLES2
    if (format->flipRB) {
        GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE));
        GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED));
    }
#endif

    for (auto const& rect : rects) {
        GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / format->bytesPerBlock));
        GLCALL(glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, rect.x1));
        GLCALL(glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, rect.y1));

        int width  = rect.x2 - rect.x1;
        int height = rect.y2 - rect.y1;
        GLCALL(glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x1, rect.y1, width, height, format->glFormat, format->glType, pixels));
    }

    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0));
    GLCALL(glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0));
    GLCALL(glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0));

    glBindTexture(GL_TEXTURE_2D, 0);

    if (m_keepDataCopy) {
        m_dataCopy.resize(stride * m_size.y);
        memcpy(m_dataCopy.data(), pixels, stride * m_size.y);
    }
}

void CTexture::destroyTexture() {
    if (m_texID) {
        GLCALL(glDeleteTextures(1, &m_texID));
        m_texID = 0;
    }

    if (m_eglImage)
        g_pHyprOpenGL->m_proc.eglDestroyImageKHR(g_pHyprOpenGL->m_eglDisplay, m_eglImage);
    m_eglImage = nullptr;
}

void CTexture::allocate() {
    if (!m_texID)
        GLCALL(glGenTextures(1, &m_texID));
}

const std::vector<uint8_t>& CTexture::dataCopy() {
    return m_dataCopy;
}
