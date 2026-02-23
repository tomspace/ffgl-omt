#pragma once
#include <FFGLSDK.h>
#include <ffglex/FFGLShader.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <libomt.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// DiscoveryManager — singleton, lives for DLL lifetime.
// Polls omt_discovery_getaddresses on a background thread.
// Plugin instances call Poll() each frame (GL thread) to get updates.
// ---------------------------------------------------------------------------
class DiscoveryManager
{
public:
    static DiscoveryManager& Instance();

    struct SourceList {
        std::vector<std::string> addresses;
        std::vector<std::string> names;
        std::vector<float>       vals;
        bool dirty = false;
    };

    // Returns current source list. dirty=true only when list changed since
    // this instance last called Poll (tracked via instanceVersion).
    SourceList Poll(uint32_t& instanceVersion);

private:
    DiscoveryManager();
    ~DiscoveryManager();
    void ThreadFunc();

    std::thread              mThread;
    std::atomic<bool>        mRunning;
    std::mutex               mMutex;
    std::vector<std::string> mAddresses;   // last known list (for change detection)
    SourceList               mCurrent;     // latest formatted list
    uint32_t                 mVersion = 0; // incremented on every change
};

// ---------------------------------------------------------------------------
// OMTReceive — FFGL Source plugin
// ---------------------------------------------------------------------------
class OMTReceive : public CFFGLPlugin
{
public:
    OMTReceive();
    ~OMTReceive() override;
    FFResult InitGL(const FFGLViewportStruct* vp) override;
    FFResult DeInitGL() override;
    FFResult ProcessOpenGL(ProcessOpenGLStruct* pGL) override;
    FFResult SetFloatParameter(unsigned int idx, float val) override;
    float    GetFloatParameter(unsigned int idx) override;
    FFResult SetTextParameter(unsigned int idx, const char* val) override;
    char*    GetTextParameter(unsigned int idx) override;

private:
    void Log(const std::string& msg);

    // GL resources
    ffglex::FFGLShader mShader;
    GLuint mVAO=0, mVBO=0;
    GLuint mHoldingTex=0;
    GLuint mVideoTex=0;
    uint32_t mVideoTexW=0, mVideoTexH=0;
    bool mReady=false, mHasFrame=false;

    // Staging buffer: frame pixels swapped out of mFrameMutex, uploaded outside lock
    std::vector<uint8_t> mUploadPixels;
    uint32_t             mUploadW=0, mUploadH=0;

    enum ParamIndex : unsigned int { PARAM_SOURCE=0, PARAM_LOGGING, PARAM_COUNT };
    std::vector<std::string> mAddresses;
    float    mSelected = 0;
    bool     mLogging  = false;
    uint32_t mSourceVersion;

    // Per-instance receive — all connection state owned by GL thread,
    // except mFrame which is shared via mFrameMutex.
    void Connect(const std::string& address);
    void DisconnectSource();
    void ReceiveThreadFunc(std::string address);

    std::thread       mReceiveThread;
    std::atomic<bool> mRunReceive;
    std::string       mConnectedAddress; // GL thread only

    struct Frame {
        uint32_t w=0, h=0;
        std::vector<uint8_t> pixels;
        bool fresh = false;
    };
    std::mutex mFrameMutex;
    Frame      mFrame;   // written by receive thread, swapped by GL thread
};
