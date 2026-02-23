#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// OMTVideoBuffer
//
// A simple double-buffer that lets one thread write pixel data (the GL render
// thread, via PBO readback) while another thread reads it (the OMT send
// thread).  "Double buffer" means the writer never blocks waiting for the
// reader – it just overwrites the back buffer and flips.
//
// Layout of each buffer slot:
//   [ width : uint32 ][ height : uint32 ][ stride : uint32 ]
//   [ raw pixel bytes … ]
//
// Format is always BGRA (4 bytes/pixel), which both FFGL and OMT support
// natively and requires no conversion.
// ---------------------------------------------------------------------------

class OMTVideoBuffer
{
public:
    OMTVideoBuffer() = default;

    // Call from the writer side (GL thread) to hand off a completed frame.
    // Copies `dataBytes` bytes from `pixels` into the back buffer, then swaps.
    void Write( uint32_t width, uint32_t height, uint32_t stride,
                const uint8_t* pixels, size_t dataBytes )
    {
        std::lock_guard< std::mutex > lock( mMutex );

        auto& buf = mBuffers[ mBackIndex ];
        buf.width  = width;
        buf.height = height;
        buf.stride = stride;
        buf.pixels.resize( dataBytes );
        std::memcpy( buf.pixels.data(), pixels, dataBytes );
        buf.fresh = true;

        // Flip back <-> front
        mBackIndex  = 1 - mBackIndex;
    }

    // Call from the reader side (OMT send thread).
    // Returns false if no new frame has arrived since the last call.
    // Swaps the pixel buffer out rather than copying — O(1), no allocations.
    bool Read( uint32_t& width, uint32_t& height, uint32_t& stride,
               std::vector< uint8_t >& outPixels )
    {
        std::lock_guard< std::mutex > lock( mMutex );

        int frontIndex = 1 - mBackIndex;
        auto& buf = mBuffers[ frontIndex ];

        if( !buf.fresh || buf.pixels.empty() )
            return false;

        width      = buf.width;
        height     = buf.height;
        stride     = buf.stride;
        outPixels.swap( buf.pixels );  // O(1) — outPixels' old buffer reused next Write
        buf.fresh  = false;
        return true;
    }

private:
    struct Slot
    {
        uint32_t             width  = 0;
        uint32_t             height = 0;
        uint32_t             stride = 0;
        std::vector<uint8_t> pixels;
        bool                 fresh  = false;
    };

    std::mutex mMutex;
    Slot       mBuffers[ 2 ];
    int        mBackIndex = 0;   // the slot the writer writes into next
};
