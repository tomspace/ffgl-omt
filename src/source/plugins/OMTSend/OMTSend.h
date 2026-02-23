#pragma once

#include <FFGLSDK.h>
#include <ffglex/FFGLScopedShaderBinding.h>
#include <ffglex/FFGLScopedSamplerActivation.h>
#include <ffglex/FFGLScopedTextureBinding.h>
#include <ffglex/FFGLShader.h>

#include "../shared/OMTVideoBuffer.h"

// These must be defined before libomt.h pulls in Windows.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <libomt.h>

#include <atomic>
#include <string>
#include <thread>

class OMTSend : public CFFGLPlugin
{
public:
    OMTSend();
    ~OMTSend() override;

    FFResult InitGL(const FFGLViewportStruct* vp) override;
    FFResult DeInitGL() override;
    FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;

    FFResult SetFloatParameter(unsigned int index, float value) override;
    float    GetFloatParameter(unsigned int index) override;
    FFResult SetTextParameter(unsigned int index, const char* value) override;
    char* GetTextParameter(unsigned int index) override;

private:
    ffglex::FFGLShader mShader;
    GLuint             mVAO = 0;
    GLuint             mVBO = 0;
    bool               mShaderReady = false;

    OMTVideoBuffer     mVideoBuffer;

    // PBO double-buffer for async GPU->CPU readback.
    // Each frame we kick off a DMA transfer into mPBO[mPBOWriteIdx],
    // then next frame we map mPBO[mPBOReadIdx] (transfer complete by then)
    // and hand the pixels to the send thread without stalling the GL thread.
    GLuint  mPBO[2] = { 0, 0 };
    int     mPBOWriteIdx = 0;      // PBO we're writing to this frame
    bool    mPBOReady = false;  // true once at least one frame has been kicked off
    size_t  mPBOSize = 0;      // current allocation size in bytes

    // Dimensions of the frame currently in-flight in the read PBO
    struct PendingFrame { uint32_t w, h, hw, stride; };
    PendingFrame mPending = {};

    // Crop buffer for when hw != w (padding columns need stripping)
    std::vector<uint8_t> mCropBuf;

    bool mDebugLogged = false;

    std::thread        mSendThread;
    std::atomic<bool>  mRunSendThread{ false };
    void SendThreadFunc();

    omt_send_t* mOMTSender = nullptr;

    enum ParamIndex : unsigned int
    {
        PARAM_SOURCE_NAME = 0,
        PARAM_QUALITY,
        PARAM_FRAMERATE,
        PARAM_LOGGING,
        PARAM_COUNT
    };

    std::string        mSourceName = "Resolume OMT";
    float              mQuality = 0.5f;
    float              mFrameRateOption = 5.0f;  // index into dropdown (5 = 60fps default)
    std::atomic<bool>  mLoggingEnabled{ false };

    // Decoded from dropdown, read atomically by send thread
    std::atomic<int>   mFrameRateN{ 60 };
    std::atomic<int>   mFrameRateD{ 1 };

    void       StartSendThread();
    void       StopSendThread();
    OMTQuality QualityEnum() const;
    void       UpdateFrameRate(float sliderValue);
};
