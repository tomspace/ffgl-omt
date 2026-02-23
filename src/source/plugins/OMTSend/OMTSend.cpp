#include "OMTSend.h"
#include <ffglex/FFGLScopedShaderBinding.h>
#include <ffglex/FFGLScopedSamplerActivation.h>
#include <ffglex/FFGLScopedTextureBinding.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>
#include <fstream>

using namespace ffglex;

static CFFGLPluginInfo PluginInfo(
    PluginFactory< OMTSend >,
    "OMTS",
    "OMT Send",
    2, 1,   // FFGL API version
    1, 0,   // Plugin version
    FF_EFFECT,
    "Send video over the network using Open Media Transport",
    "openmediatransport.org"
);

static const char kVertexShader[] = R"(#version 410 core
uniform vec2 MaxUV;
layout(location = 0) in vec4 vPosition;
layout(location = 1) in vec2 vUV;
out vec2 uv;
void main()
{
    gl_Position = vPosition;
    uv = vUV * MaxUV;
}
)";

static const char kFragmentShader[] = R"(#version 410 core
uniform sampler2D InputTexture;
in  vec2 uv;
out vec4 fragColor;
void main()
{
    fragColor = texture(InputTexture, uv);
}
)";

OMTSend::OMTSend()
    : CFFGLPlugin()
{
    SetMinInputs( 1 );
    SetMaxInputs( 1 );

    SetParamInfof( PARAM_SOURCE_NAME, "Source Name", FF_TYPE_TEXT );
    SetParamInfof( PARAM_QUALITY,     "Quality",     FF_TYPE_STANDARD );

    // Frame rate as a named dropdown - values 0..4 map to the 5 options
    SetOptionParamInfo( PARAM_FRAMERATE, "Frame Rate", 6, 5.0f );
    SetParamElementInfo( PARAM_FRAMERATE, 0, "24 fps",      0.0f );
    SetParamElementInfo( PARAM_FRAMERATE, 1, "25 fps",      1.0f );
    SetParamElementInfo( PARAM_FRAMERATE, 2, "29.97 fps",   2.0f );
    SetParamElementInfo( PARAM_FRAMERATE, 3, "30 fps",      3.0f );
    SetParamElementInfo( PARAM_FRAMERATE, 4, "50 fps",      4.0f );
    SetParamElementInfo( PARAM_FRAMERATE, 5, "60 fps",      5.0f );

    SetParamInfof( PARAM_LOGGING, "Enable Logging", FF_TYPE_BOOLEAN );

    mSourceName = "Resolume OMT";
    UpdateFrameRate( 5.0f );  // default to 60fps
}

OMTSend::~OMTSend()
{
}

FFResult OMTSend::InitGL( const FFGLViewportStruct* vp )
{
    if( !mShader.Compile( kVertexShader, kFragmentShader ) )
    {
        DeInitGL();
        return FF_FAIL;
    }

    static const float kQuadVerts[] = {
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
        -1.f,  1.f,  0.f, 1.f,
         1.f,  1.f,  1.f, 1.f,
    };
    glGenVertexArrays( 1, &mVAO );
    glGenBuffers( 1, &mVBO );
    glBindVertexArray( mVAO );
    glBindBuffer( GL_ARRAY_BUFFER, mVBO );
    glBufferData( GL_ARRAY_BUFFER, sizeof( kQuadVerts ), kQuadVerts, GL_STATIC_DRAW );
    glEnableVertexAttribArray( 0 );
    glVertexAttribPointer( 0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof( float ), (void*)0 );
    glEnableVertexAttribArray( 1 );
    glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof( float ), (void*)( 2 * sizeof( float ) ) );
    glBindBuffer( GL_ARRAY_BUFFER, 0 );
    glBindVertexArray( 0 );

    mShaderReady = true;
    glGenBuffers( 2, mPBO );
    return FF_SUCCESS;
}

FFResult OMTSend::DeInitGL()
{
    StopSendThread();  // thread destroys sender before exiting
    mShader.FreeGLResources();
    if( mVAO ) { glDeleteVertexArrays( 1, &mVAO ); mVAO = 0; }
    if( mVBO ) { glDeleteBuffers( 1, &mVBO ); mVBO = 0; }
    if( mPBO[0] ) { glDeleteBuffers( 2, mPBO ); mPBO[0] = mPBO[1] = 0; }
    mPBOReady = false;
    mPBOSize  = 0;
    mShaderReady = false;
    return FF_SUCCESS;
}

FFResult OMTSend::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
    if( !mShaderReady || pGL->numInputTextures < 1 || !pGL->inputTextures[ 0 ] )
        return FF_FAIL;

    // Start the send thread on the first frame - by this point Resolume has
    // finished setting all parameters (including Source Name) so we get the
    // correct name from the start rather than an empty string.
    if( !mRunSendThread )
        StartSendThread();

    const FFGLTextureStruct& inputTex = *pGL->inputTextures[ 0 ];
    if( inputTex.Handle == 0 || inputTex.Width == 0 || inputTex.Height == 0 ||
        inputTex.HardwareWidth == 0 || inputTex.HardwareHeight == 0 )
        return FF_SUCCESS;  // texture not ready yet – skip silently

    // Use actual video dimensions (not HardwareWidth which may be power-of-2 padded)
    const uint32_t w      = inputTex.Width;
    const uint32_t h      = inputTex.Height;
    const uint32_t stride = w * 4;  // BGRA = 4 bytes/pixel

    // 1. Pass-through render
    {
        ScopedShaderBinding shaderBinding( mShader.GetGLID() );
        ScopedSamplerActivation sampler( 0 );
        ScopedTextureBinding texBinding( GL_TEXTURE_2D, inputTex.Handle );

        FFGLTexCoords maxCoords = GetMaxGLTexCoords( inputTex );
        mShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
        mShader.Set( "InputTexture", 0 );

        glBindVertexArray( mVAO );
        glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
        glBindVertexArray( 0 );
    }

    // -----------------------------------------------------------------------
    // Async GPU->CPU readback using two PBOs (double-buffer).
    //
    // This frame:
    //   1. Map the READ PBO (filled by last frame's glGetTexImage) and hand
    //      the pixels to the send thread — no GPU stall because the DMA
    //      completed during the intervening frame.
    //   2. Bind the WRITE PBO and call glGetTexImage to kick off the next
    //      async DMA transfer — returns immediately.
    //   3. Swap read/write indices for next frame.
    //
    // On the very first frame mPBOReady is false so we skip step 1.
    // -----------------------------------------------------------------------

    const uint32_t hw     = inputTex.HardwareWidth;
    const uint32_t hh     = inputTex.HardwareHeight;
    const size_t   hwSize = (size_t)hw * hh * 4;

    // Reallocate both PBOs if the texture size changed
    if( hwSize != mPBOSize )
    {
        for( int i = 0; i < 2; ++i )
        {
            glBindBuffer( GL_PIXEL_PACK_BUFFER, mPBO[i] );
            glBufferData( GL_PIXEL_PACK_BUFFER, (GLsizeiptr)hwSize, nullptr, GL_STREAM_READ );
        }
        glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
        mPBOSize  = hwSize;
        mPBOReady = false;  // discard any in-flight frame at old size
    }

    const int readIdx  = mPBOWriteIdx;        // what we wrote last frame
    const int writeIdx = 1 - mPBOWriteIdx;    // what we write this frame

    // --- Step 1: read last frame's PBO ---
    if( mPBOReady )
    {
        const PendingFrame& pf = mPending;

        glBindBuffer( GL_PIXEL_PACK_BUFFER, mPBO[readIdx] );
        const uint8_t* src = reinterpret_cast<const uint8_t*>(
            glMapBuffer( GL_PIXEL_PACK_BUFFER, GL_READ_ONLY ) );

        if( src )
        {
            // Flip rows: glGetTexImage reads bottom-to-top, OMT expects top-to-bottom.
            // This also handles the hw != w padding case since we copy pf.stride bytes
            // from each source row (skipping any padding columns on the right).
            mCropBuf.resize( (size_t)pf.stride * pf.h );
            for( uint32_t row = 0; row < pf.h; ++row )
                std::memcpy( mCropBuf.data() + row * pf.stride,
                             src + ( pf.h - 1 - row ) * pf.hw * 4,
                             pf.stride );
            mVideoBuffer.Write( pf.w, pf.h, pf.stride, mCropBuf.data(), mCropBuf.size() );
            glUnmapBuffer( GL_PIXEL_PACK_BUFFER );
        }
        glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
    }

    // --- Step 2: kick off async DMA into write PBO ---
    glBindBuffer( GL_PIXEL_PACK_BUFFER, mPBO[writeIdx] );
    glBindTexture( GL_TEXTURE_2D, inputTex.Handle );
    glGetTexImage( GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr ); // nullptr = write to PBO
    glBindTexture( GL_TEXTURE_2D, 0 );
    glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );

    // Save dimensions for next frame's read step
    mPending     = { w, h, hw, stride };
    mPBOWriteIdx = writeIdx;
    mPBOReady    = true;

    // Debug: log once to confirm readback is working
    if( !mDebugLogged && mPBOReady && mLoggingEnabled )
    {
        mDebugLogged = true;
        static int sAnchor = 0;
        wchar_t path[ MAX_PATH ] = {};
        HMODULE hm = nullptr;
        if( GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)&sAnchor, &hm ) )
        {
            GetModuleFileNameW( hm, path, MAX_PATH );
            wchar_t* sl = wcsrchr( path, L'\\' );
            if( sl ) wcscpy_s( sl + 1, MAX_PATH - (sl - path) - 1, L"omtsend_debug.txt" );
            std::ofstream dbg( path, std::ios::app );
            if( dbg )
                dbg << "PBO readback active: w=" << w << " h=" << h
                    << " hw=" << hw << " hh=" << hh << "\n";
        }
    }

    return FF_SUCCESS;
}

FFResult OMTSend::SetFloatParameter( unsigned int index, float value )
{
    if( index == PARAM_QUALITY )
    {
        mQuality = std::clamp( value, 0.0f, 1.0f );
        return FF_SUCCESS;
    }
    if( index == PARAM_FRAMERATE )
    {
        mFrameRateOption = value;
        UpdateFrameRate( mFrameRateOption );
        return FF_SUCCESS;
    }
    if( index == PARAM_LOGGING )
    {
        mLoggingEnabled = ( value > 0.5f );
        return FF_SUCCESS;
    }
    return FF_FAIL;
}

float OMTSend::GetFloatParameter( unsigned int index )
{
    if( index == PARAM_QUALITY )   return mQuality;
    if( index == PARAM_FRAMERATE ) return mFrameRateOption;
    if( index == PARAM_LOGGING )   return mLoggingEnabled ? 1.0f : 0.0f;
    return 0.0f;
}

FFResult OMTSend::SetTextParameter( unsigned int index, const char* value )
{
    if( index == PARAM_SOURCE_NAME && value )
    {
        // Resolume sends "" on load before the user has set anything;
        // treat that as "use the default" rather than blanking the name.
        std::string newName = ( value[0] != '\0' ) ? value : "Resolume OMT";
        if( mSourceName != newName )
        {
            mSourceName = newName;
            // Restart the send thread so omt_send_create picks up the new name
            if( mRunSendThread )
            {
                StopSendThread();
                mDebugLogged = false;
                StartSendThread();
            }
        }
        return FF_SUCCESS;
    }
    return FF_FAIL;
}

char* OMTSend::GetTextParameter( unsigned int index )
{
    if( index == PARAM_SOURCE_NAME ) return const_cast< char* >( mSourceName.c_str() );
    return nullptr;
}



void OMTSend::StartSendThread()
{
    if( mRunSendThread ) return;
    mRunSendThread = true;
    mSendThread    = std::thread( &OMTSend::SendThreadFunc, this );
}

void OMTSend::StopSendThread()
{
    mRunSendThread = false;
    if( mSendThread.joinable() )
        mSendThread.join();
}

void OMTSend::SendThreadFunc()
{
    // Resolve the folder where our plugin DLL lives.
    // Using FROM_ADDRESS on a local static ensures we get the plugin's own path,
    // not libomt.dll which may be in a different directory.
    std::wstring pluginDir;
    {
        static int sAnchor = 0;
        wchar_t path[ MAX_PATH ] = {};
        HMODULE hm = nullptr;
        if( GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)&sAnchor, &hm ) )
        {
            GetModuleFileNameW( hm, path, MAX_PATH );
            wchar_t* sl = wcsrchr( path, L'\\' );
            if( sl ) pluginDir = std::wstring( path, sl + 1 );
        }
        if( pluginDir.empty() ) pluginDir = L".\\";
    }
    std::wstring vmxPath  = pluginDir + L"libvmx.dll";
    std::wstring omtLog   = pluginDir + L"libomt_send.log";
    std::wstring debugLog = pluginDir + L"omtsend_debug.txt";

    // Pre-load libvmx.dll explicitly — .NET NativeAOT P/Invoke won't find it otherwise
    HMODULE hvmx = LoadLibraryW( vmxPath.c_str() );
    if( mLoggingEnabled )
    {
        std::ofstream dbg( debugLog, std::ios::app );
        if( dbg )
        {
            dbg << "libvmx load: " << ( hvmx ? "OK" : "FAILED" )
                << " err=" << GetLastError() << "\n";
        }
    }

    // Direct OMT's own log to the plugin folder (only if logging enabled)
    if( mLoggingEnabled )
    {
        std::string omtLogA( omtLog.begin(), omtLog.end() );
        omt_setloggingfilename( omtLogA.c_str() );
    }

    mOMTSender = omt_send_create( mSourceName.c_str(), QualityEnum() );
    if( !mOMTSender )
    {
        if( mLoggingEnabled )
        {
            std::ofstream dbg( debugLog, std::ios::app );
            if( dbg ) dbg << "omt_send_create FAILED for name='" << mSourceName << "'\n";
        }
        return;
    }

    if( mLoggingEnabled )
    {
        char addr[1024] = {};
        omt_send_getaddress( mOMTSender, addr, sizeof(addr) );
        std::ofstream dbg( debugLog, std::ios::app );
        if( dbg ) dbg << "omt_send_create OK, address='" << addr << "'\n";
    }

    std::vector< uint8_t > pixelBuf;

    while( mRunSendThread )
    {
        uint32_t w = 0, h = 0, stride = 0;

        if( mVideoBuffer.Read( w, h, stride, pixelBuf ) )
        {
            OMTMediaFrame frame = {};
            frame.Type          = OMTFrameType_Video;
            frame.Codec         = OMTCodec_BGRA;
            frame.Width         = (int)w;
            frame.Height        = (int)h;
            frame.Stride        = (int)stride;
            frame.FrameRateN    = mFrameRateN.load();
            frame.FrameRateD    = mFrameRateD.load();
            frame.Timestamp     = -1;
            frame.AspectRatio   = (float)w / (float)h;
            frame.ColorSpace    = OMTColorSpace_BT709;
            frame.Flags         = OMTVideoFlags_Alpha;
            frame.Data          = pixelBuf.data();
            frame.DataLength    = (int)pixelBuf.size();

            omt_send( mOMTSender, &frame );
        }
        else
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    }

    omt_send_destroy( mOMTSender );
    mOMTSender = nullptr;
}

OMTQuality OMTSend::QualityEnum() const
{
    if( mQuality < 0.33f ) return OMTQuality_Low;
    if( mQuality < 0.67f ) return OMTQuality_Medium;
    return OMTQuality_High;
}

void OMTSend::UpdateFrameRate( float optionValue )
{
    // optionValue is the element value set in SetParamElementInfo (0..4)
    int idx = (int)( optionValue + 0.5f );
    switch( idx )
    {
    case 0:  mFrameRateN = 24;    mFrameRateD = 1;    break;  // 24 fps
    case 1:  mFrameRateN = 25;    mFrameRateD = 1;    break;  // 25 fps
    case 2:  mFrameRateN = 30000; mFrameRateD = 1001; break;  // 29.97 fps
    case 3:  mFrameRateN = 30;    mFrameRateD = 1;    break;  // 30 fps
    case 4:  mFrameRateN = 50;    mFrameRateD = 1;    break;  // 50 fps
    default: mFrameRateN = 60;    mFrameRateD = 1;    break;  // 60 fps
    }
}
