#include "MinTest.h"
#include <ffglex/FFGLScopedShaderBinding.h>
#include <fstream>
#include <string>

using namespace ffglex;

static CFFGLPluginInfo PluginInfo(
    PluginFactory< MinTest >,
    "MTST", "Min Test", 2, 1, 1, 1,
    FF_EFFECT,
    "Minimal test plugin - outputs solid red",
    "test"
);

static const char kVert[] = R"(#version 410 core
layout(location=0) in vec2 vPos;
void main() { gl_Position = vec4(vPos, 0.0, 1.0); }
)";

static const char kFrag[] = R"(#version 410 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0, 0.0, 0.0, 1.0); }
)";

static void MLog(const std::string& msg)
{
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring p = std::wstring(tmp) + L"mintest_debug.txt";
    std::ofstream f(std::string(p.begin(), p.end()), std::ios::app);
    if (f) { f << msg << "\n"; f.flush(); }
}

MinTest::MinTest() : CFFGLPlugin()
{
    SetMinInputs(1); SetMaxInputs(1);
    MLog("=== MinTest constructor ===");
}

MinTest::~MinTest()
{
    MLog("=== MinTest destructor ===");
}

FFResult MinTest::InitGL(const FFGLViewportStruct* vp)
{
    MLog("=== MinTest InitGL ===");
    if (!mShader.Compile(kVert, kFrag)) { Log("Shader FAILED"); return FF_FAIL; }
    MLog("Shader OK");

    float verts[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glGenVertexArrays(1, &mVAO);
    glGenBuffers(1, &mVBO);
    glBindVertexArray(mVAO);
    glBindBuffer(GL_ARRAY_BUFFER, mVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    mReady = true;
    MLog("InitGL complete");
    return FF_SUCCESS;
}

FFResult MinTest::DeInitGL()
{
    MLog("=== MinTest DeInitGL ===");
    mShader.FreeGLResources();
    if (mVAO) { glDeleteVertexArrays(1, &mVAO); mVAO = 0; }
    if (mVBO) { glDeleteBuffers(1, &mVBO); mVBO = 0; }
    mReady = false;
    return FF_SUCCESS;
}

FFResult MinTest::ProcessOpenGL(ProcessOpenGLStruct* pGL)
{
    static int n = 0;
    if (++n <= 5 || n % 300 == 0)
        MLog("ProcessOpenGL #" + std::to_string(n) + " ready=" + std::to_string(mReady));

    if (!mReady) return FF_FAIL;

    ScopedShaderBinding bind(mShader.GetGLID());
    glBindVertexArray(mVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    return FF_SUCCESS;
}
