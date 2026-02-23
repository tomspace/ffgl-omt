#pragma once
#include <FFGLSDK.h>
#include <ffglex/FFGLShader.h>

class MinTest : public CFFGLPlugin
{
public:
    MinTest();
    ~MinTest() override;
    FFResult InitGL( const FFGLViewportStruct* vp ) override;
    FFResult DeInitGL() override;
    FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;

private:
    ffglex::FFGLShader mShader;
    GLuint mVAO = 0;
    GLuint mVBO = 0;
    bool   mReady = false;
};
