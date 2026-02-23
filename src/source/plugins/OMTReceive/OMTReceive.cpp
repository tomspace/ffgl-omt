#include "OMTReceive.h"
#include "HoldingImage.h"
#include <ffglex/FFGLScopedShaderBinding.h>
#include <ffglex/FFGLScopedSamplerActivation.h>
#include <ffglex/FFGLScopedTextureBinding.h>
#include <fstream>
#include <string>
#include <chrono>
#include <cstring>
#include <algorithm>

using namespace ffglex;

static CFFGLPluginInfo PluginInfo(
    PluginFactory< OMTReceive >,
    "OMRV", "OMT Receive", 2, 1, 0, 0,
    FF_SOURCE,
    "Receive video over the network using Open Media Transport",
    "openmediatransport.org"
);

static const char kVert[] = R"(#version 410 core
layout(location=0) in vec2 vPos;
layout(location=1) in vec2 vUV;
out vec2 uv;
void main() { gl_Position = vec4(vPos,0,1); uv = vec2(vUV.x, 1.0-vUV.y); }
)";

static const char kFrag[] = R"(#version 410 core
uniform sampler2D tex;
in vec2 uv;
out vec4 fragColor;
void main() { fragColor = texture(tex, uv); }
)";

// ---------------------------------------------------------------------------
// Logging — DLL-relative, per-instance flag, thread-safe path init
// ---------------------------------------------------------------------------
static std::string GetDllDir()
{
    static int sAnchor = 0;
    wchar_t path[MAX_PATH] = {};
    HMODULE hm = nullptr;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)&sAnchor, &hm))
    {
        GetModuleFileNameW(hm, path, MAX_PATH);
        wchar_t* sl = wcsrchr(path, L'\\');
        if(sl) *(sl+1) = L'\0';
        return std::string(path, path + wcslen(path));
    }
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::string(tmp, tmp + wcslen(tmp));
}

// Log path resolved once, lazily, under a mutex
static std::mutex  sLogMutex;
static std::string sLogPath;

static void MLog(bool enabled, const std::string& msg)
{
    if(!enabled) return;
    std::lock_guard<std::mutex> lk(sLogMutex);
    if(sLogPath.empty())
        sLogPath = GetDllDir() + "OMTReceive.log";
    std::ofstream f(sLogPath, std::ios::app);
    if(f) { f << msg << "\n"; f.flush(); }
}

static void EnsureLibvmx()
{
    if(GetModuleHandleW(L"libvmx.dll")) return;
    wchar_t path[MAX_PATH]={};
    HMODULE h = GetModuleHandleW(L"libomt.dll");
    if(h && GetModuleFileNameW(h, path, MAX_PATH)) {
        wchar_t* sl = wcsrchr(path, L'\\');
        if(sl) wcscpy_s(sl+1, MAX_PATH-(sl-path)-1, L"libvmx.dll");
        LoadLibraryW(path);
    }
}

void OMTReceive::Log(const std::string& msg)
{
    MLog(mLogging, msg);
}

// ---------------------------------------------------------------------------
// DiscoveryManager
// ---------------------------------------------------------------------------
DiscoveryManager& DiscoveryManager::Instance()
{
    static DiscoveryManager inst;
    return inst;
}

DiscoveryManager::DiscoveryManager() : mRunning(false)
{
    mRunning = true;
    mThread = std::thread(&DiscoveryManager::ThreadFunc, this);
}

DiscoveryManager::~DiscoveryManager()
{
    mRunning = false;
    if(mThread.joinable()) mThread.join();
}

DiscoveryManager::SourceList DiscoveryManager::Poll(uint32_t& instanceVersion)
{
    std::lock_guard<std::mutex> lk(mMutex);
    SourceList result = mCurrent;
    result.dirty = (instanceVersion != mVersion);
    instanceVersion = mVersion;
    return result;
}

void DiscoveryManager::ThreadFunc()
{
    while(mRunning)
    {
        int count=0;
        char** addrs = omt_discovery_getaddresses(&count);
        std::vector<std::string> found;
        if(count > 0 && addrs)
            for(int i=0; i<count; ++i)
                if(addrs[i]) found.push_back(addrs[i]);

        {
            std::lock_guard<std::mutex> lk(mMutex);
            if(found != mAddresses)
            {
                mAddresses = found;
                mCurrent.addresses = found;
                mCurrent.names.clear();
                mCurrent.vals.clear();
                if(found.empty()) {
                    mCurrent.names = {"No sources"};
                    mCurrent.vals  = {0.0f};
                } else {
                    for(int i=0; i<(int)found.size(); ++i) {
                        mCurrent.names.push_back(found[i]);
                        mCurrent.vals.push_back((float)i);
                    }
                }
                mVersion++;
            }
        }

        for(int i=0; i<30 && mRunning; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---------------------------------------------------------------------------
// OMTReceive
// ---------------------------------------------------------------------------
OMTReceive::OMTReceive() : CFFGLPlugin(), mRunReceive(false), mSourceVersion(0xFFFFFFFF)
{
    SetMinInputs(0); SetMaxInputs(0);
    SetOptionParamInfo(PARAM_SOURCE, "Source", 1, 0.0f);
    SetParamElementInfo(PARAM_SOURCE, 0, "Scanning...", 0.0f);
    SetParamInfof(PARAM_LOGGING, "Logging", FF_TYPE_BOOLEAN);
}

OMTReceive::~OMTReceive()
{
    DisconnectSource();
}

FFResult OMTReceive::InitGL(const FFGLViewportStruct* vp)
{
    Log("=== InitGL ===");
    if(!mShader.Compile(kVert, kFrag)) { Log("shader FAIL"); DeInitGL(); return FF_FAIL; }

    float verts[] = { -1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1 };
    glGenVertexArrays(1,&mVAO); glGenBuffers(1,&mVBO);
    glBindVertexArray(mVAO);
    glBindBuffer(GL_ARRAY_BUFFER, mVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glBindVertexArray(0);

    // Upload holding image (raw BGRA, embedded in header)
    glGenTextures(1, &mHoldingTex);
    glBindTexture(GL_TEXTURE_2D, mHoldingTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kHoldingW, kHoldingH, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, kHoldingData);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Video texture - 1x1 placeholder, grown on first frame
    glGenTextures(1, &mVideoTex);
    glBindTexture(GL_TEXTURE_2D, mVideoTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    mReady = true;
    Log("InitGL complete");
    return FF_SUCCESS;
}

FFResult OMTReceive::DeInitGL()
{
    DisconnectSource();
    mShader.FreeGLResources();
    if(mVAO)        { glDeleteVertexArrays(1,&mVAO); mVAO=0; }
    if(mVBO)        { glDeleteBuffers(1,&mVBO); mVBO=0; }
    if(mHoldingTex) { glDeleteTextures(1,&mHoldingTex); mHoldingTex=0; }
    if(mVideoTex)   { glDeleteTextures(1,&mVideoTex); mVideoTex=0; }
    mVideoTexW=mVideoTexH=0; mReady=false;
    return FF_SUCCESS;
}

FFResult OMTReceive::ProcessOpenGL(ProcessOpenGLStruct* pGL)
{
    if(!mReady) return FF_SUCCESS;

    // Apply discovery updates on GL thread (safe to call SetParamElements here)
    auto sl = DiscoveryManager::Instance().Poll(mSourceVersion);
    if(sl.dirty)
    {
        Log("sources: " + std::to_string(sl.addresses.size()));
        mAddresses = sl.addresses;
        SetParamElements(PARAM_SOURCE, sl.names, sl.vals, true);

        // Auto-connect to sole source
        if(sl.addresses.size() == 1 && mConnectedAddress != sl.addresses[0])
        {
            Log("auto-connect: " + sl.addresses[0]);
            Connect(sl.addresses[0]);
        }
        // Reconnect if selected index still valid (e.g. after source list refresh)
        else if(!sl.addresses.empty())
        {
            int idx = (int)(mSelected + 0.5f);
            if(idx >= 0 && idx < (int)sl.addresses.size() &&
               sl.addresses[idx] != mConnectedAddress)
                Connect(sl.addresses[idx]);
        }
    }

    // Check for reconnect: receive thread may have exited due to source disappearing
    if(!mConnectedAddress.empty() && !mReceiveThread.joinable())
    {
        Log("source lost, reconnecting: " + mConnectedAddress);
        std::string addr = mConnectedAddress;
        mConnectedAddress.clear();
        Connect(addr);
    }

    // Pull latest frame out of the shared buffer with minimal lock time —
    // swap the pixel vector rather than copying, so we never block the
    // receive thread for more than a pointer swap.
    {
        std::lock_guard<std::mutex> lk(mFrameMutex);
        if(mFrame.fresh)
        {
            mUploadW = mFrame.w;
            mUploadH = mFrame.h;
            mUploadPixels.swap(mFrame.pixels); // O(1) - no copy
            mFrame.fresh = false;
        }
    }

    // Upload to GPU outside the lock - receive thread is free to write next frame
    if(mUploadW && mUploadH && !mUploadPixels.empty())
    {
        glBindTexture(GL_TEXTURE_2D, mVideoTex);
        if(mUploadW != mVideoTexW || mUploadH != mVideoTexH) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                mUploadW, mUploadH, 0, GL_BGRA, GL_UNSIGNED_BYTE, mUploadPixels.data());
            mVideoTexW=mUploadW; mVideoTexH=mUploadH;
            Log("video tex " + std::to_string(mVideoTexW) + "x" + std::to_string(mVideoTexH));
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                mUploadW, mUploadH, GL_BGRA, GL_UNSIGNED_BYTE, mUploadPixels.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        mHasFrame = true;
        mUploadW = mUploadH = 0;
    }

    // Draw — live video once we have a frame, holding image until then
    GLuint drawTex = mHasFrame ? mVideoTex : mHoldingTex;
    if(drawTex)
    {
        ScopedShaderBinding sb(mShader.GetGLID());
        ScopedSamplerActivation sa(0);
        ScopedTextureBinding tb(GL_TEXTURE_2D, drawTex);
        mShader.Set("tex", 0);
        glBindVertexArray(mVAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
    }

    return FF_SUCCESS;
}

FFResult OMTReceive::SetFloatParameter(unsigned int idx, float val)
{
    if(idx == PARAM_SOURCE) {
        mSelected = val;
        int i = (int)(val + 0.5f);
        Log("source selected: " + std::to_string(i));
        if(i >= 0 && i < (int)mAddresses.size())
            Connect(mAddresses[i]);
        return FF_SUCCESS;
    }
    if(idx == PARAM_LOGGING) {
        mLogging = (val > 0.5f);
        if(mLogging) Log("=== Logging enabled ===");
        return FF_SUCCESS;
    }
    return FF_FAIL;
}

float OMTReceive::GetFloatParameter(unsigned int idx)
{
    if(idx == PARAM_SOURCE)  return mSelected;
    if(idx == PARAM_LOGGING) return mLogging ? 1.0f : 0.0f;
    return 0;
}

FFResult OMTReceive::SetTextParameter(unsigned int, const char*) { return FF_FAIL; }
char*    OMTReceive::GetTextParameter(unsigned int) { return nullptr; }

// ---------------------------------------------------------------------------
// Per-instance receive connection
// ---------------------------------------------------------------------------
void OMTReceive::Connect(const std::string& address)
{
    if(address == mConnectedAddress) return;
    DisconnectSource();
    mHasFrame = false;
    Log("Connecting: " + address);
    mConnectedAddress = address;
    mRunReceive = true;
    mReceiveThread = std::thread(&OMTReceive::ReceiveThreadFunc, this, address);
}

void OMTReceive::DisconnectSource()
{
    mRunReceive = false;
    if(mReceiveThread.joinable()) mReceiveThread.join();
    mConnectedAddress.clear();
}

void OMTReceive::ReceiveThreadFunc(std::string address)
{
    EnsureLibvmx();

    // mReceiver is local — never accessed outside this thread
    omt_receive_t* receiver = omt_receive_create(address.c_str(),
        OMTFrameType_Video, OMTPreferredVideoFormat_BGRA, OMTReceiveFlags_None);
    Log("[RX] " + address + ": " + (receiver ? "OK" : "FAIL"));
    if(!receiver) {
        // Don't touch mConnectedAddress from this thread — GL thread owns it.
        // Setting mRunReceive=false signals Connect() / reconnect logic.
        mRunReceive = false;
        return;
    }

    // Pre-size the pixel buffer to avoid per-frame allocation after the first frame
    std::vector<uint8_t> stagingPixels;

    bool firstFrame = true;
    while(mRunReceive)
    {
        OMTMediaFrame* frame = omt_receive(receiver, OMTFrameType_Video, 100);
        if(!frame || !frame->Data || frame->DataLength <= 0)
            continue;

        if(firstFrame) {
            firstFrame = false;
            Log("[RX] first frame " + std::to_string(frame->Width) + "x" + std::to_string(frame->Height));
            stagingPixels.reserve(frame->DataLength);
        }

        // Copy into staging outside the lock
        stagingPixels.resize(frame->DataLength);
        std::memcpy(stagingPixels.data(), frame->Data, frame->DataLength);

        // Swap staging into the shared frame — O(1), hold lock as briefly as possible
        {
            std::lock_guard<std::mutex> lk(mFrameMutex);
            mFrame.w = (uint32_t)frame->Width;
            mFrame.h = (uint32_t)frame->Height;
            mFrame.pixels.swap(stagingPixels); // O(1)
            mFrame.fresh = true;
        }
        // stagingPixels now holds the previous (stale) buffer — reused next iteration
    }

    omt_receive_destroy(receiver);
    Log("[RX] disconnected: " + address);
}
