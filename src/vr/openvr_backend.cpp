#include "vr/openvr_backend.h"

#include <windows.h>

#include <d3d11.h>
// (The project builds for Direct3D 7, DIRECT3D_VERSION 0x0700; this file needs Direct3D 9's
// declarations to share the eyes with dxwrapper's device.)
#undef DIRECT3D_VERSION
#define DIRECT3D_VERSION 0x0900
#include <d3d9.h>
#include <dxgi.h>
#include <openvr.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/paths.h"

namespace bw::vr::openvr {

namespace {

bool g_active = false;
::vr::IVRSystem* g_sys = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Texture2D* g_tex[2] = {nullptr, nullptr};
int g_tex_w[2] = {0, 0}, g_tex_h[2] = {0, 0};
bool g_eye_ready[2] = {false, false};

// The eyes shared with Direct3D 9 (the game drawing through dxwrapper): a D3D9Ex render-target
// texture per eye, opened in D3D11. Each frame the eye's crop is copied into it on the GPU.
struct SharedEye {
    IDirect3DDevice9* dev9 = nullptr;  // (dxwrapper's; not ours to release)
    IDirect3DTexture9* tex9 = nullptr;
    IDirect3DSurface9* surf9 = nullptr;
    ID3D11Texture2D* tex11 = nullptr;
    int w = 0, h = 0;
    void release() {
        if (tex11) tex11->Release();
        if (surf9) surf9->Release();
        if (tex9) tex9->Release();
        tex11 = nullptr, surf9 = nullptr, tex9 = nullptr, dev9 = nullptr, w = h = 0;
    }
} g_shared[2];
IDirect3DQuery9* g_shared_done = nullptr;  // (an event query on dxwrapper's device: the copies are finished)
IDirect3DDevice9* g_query_dev = nullptr;
double g_share_ms = 0;
unsigned g_shared_frames = 0;
bool g_shared_left = false;  // (the left eye is copied and waits for the right)

::vr::Compositor_CumulativeStats g_last_stats{};
unsigned g_waits = 0, g_invalid_poses = 0;
int g_last_tracking_result = 0;
bool g_hmd_connected = false;
double g_wait_ms = 0, g_upload_ms = 0;
LARGE_INTEGER g_freq;

double ms_since(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    return double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

// OpenVR is right-handed with -Z forward; the engine is left-handed with +Z forward.
// Converting a vector flips Z; converting a rotation is S*R*S with S = diag(1, 1, -1).
Vec3 to_engine(float x, float y, float z) { return {x, y, -z}; }

Pose pose_from(const ::vr::HmdMatrix34_t& m) {
    Pose p;
    p.rot.right = {m.m[0][0], m.m[1][0], -m.m[2][0]};
    p.rot.up = {m.m[0][1], m.m[1][1], -m.m[2][1]};
    p.rot.fwd = {-m.m[0][2], -m.m[1][2], m.m[2][2]};
    p.pos = to_engine(m.m[0][3], m.m[1][3], m.m[2][3]);
    return p;
}

bool create_device() {
    int32_t adapter_index = -1;
    g_sys->GetDXGIOutputInfo(&adapter_index);
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        if (adapter_index >= 0) factory->EnumAdapters1(UINT(adapter_index), &adapter);
        factory->Release();
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   levels, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx);
    if (adapter) adapter->Release();
    if (FAILED(hr)) {
        BWLOG("openvr: D3D11CreateDevice failed %08lX", hr);
        return false;
    }
    return true;
}

ID3D11Texture2D* g_upload[2] = {nullptr, nullptr};  // (CPU-writable: filled straight from the eye's render target, then copied on the GPU)
bool ensure_texture(int eye, int w, int h) {
    if (g_tex[eye] && g_upload[eye] && g_tex_w[eye] == w && g_tex_h[eye] == h) return true;
    if (g_tex[eye]) g_tex[eye]->Release();
    if (g_upload[eye]) g_upload[eye]->Release();
    g_tex[eye] = g_upload[eye] = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // same byte order as the D3D7 X8R8G8B8 surface
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_tex[eye]))) return false;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_upload[eye]))) return false;
    g_tex_w[eye] = w;
    g_tex_h[eye] = h;
    BWLOG("openvr: eye %d texture %dx%d", eye, w, h);
    return true;
}

// SteamVR Input handles.
bool g_input = false;
::vr::VRActionSetHandle_t g_set = 0;
::vr::VRActionHandle_t g_aim = 0, g_grip_pose = 0, g_trigger = 0, g_squeeze = 0, g_a = 0, g_b = 0, g_stick = 0,
                       g_stick_click = 0, g_skeleton[2] = {0, 0}, g_haptic = 0, g_squeeze_click = 0;
::vr::VRInputValueHandle_t g_hand_src[2] = {0, 0};

void init_input() {
    ::vr::IVRInput* in = ::vr::VRInput();
    if (!in) return;
    const std::wstring wpath = paths::bwvr_dir() + L"input\\actions.json";
    char path[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path, sizeof(path), nullptr, nullptr);
    ::vr::EVRInputError e = in->SetActionManifestPath(path);
    if (e != ::vr::VRInputError_None) {
        BWLOG("openvr: SetActionManifestPath(%s) failed: %d", path, int(e));
        return;
    }
    in->GetActionSetHandle("/actions/god", &g_set);
    in->GetActionHandle("/actions/god/in/aim", &g_aim);
    in->GetActionHandle("/actions/god/in/grip_pose", &g_grip_pose);
    in->GetActionHandle("/actions/god/in/trigger", &g_trigger);
    in->GetActionHandle("/actions/god/in/squeeze", &g_squeeze);
    // (Controllers whose grip is a button, WMR and Vive Cosmos, bind it here: held, it is a grip
    // squeezed all the way.)
    in->GetActionHandle("/actions/god/in/squeeze_click", &g_squeeze_click);
    in->GetActionHandle("/actions/god/in/a", &g_a);
    in->GetActionHandle("/actions/god/in/b", &g_b);
    in->GetActionHandle("/actions/god/in/stick", &g_stick);
    in->GetActionHandle("/actions/god/in/stick_click", &g_stick_click);
    in->GetActionHandle("/actions/god/in/skeleton_left", &g_skeleton[0]);
    in->GetActionHandle("/actions/god/in/skeleton_right", &g_skeleton[1]);
    in->GetActionHandle("/actions/god/out/haptic", &g_haptic);
    in->GetInputSourceHandle("/user/hand/left", &g_hand_src[0]);
    in->GetInputSourceHandle("/user/hand/right", &g_hand_src[1]);
    g_input = true;
    BWLOG("openvr: input manifest loaded (%s)", path);
}

}  // namespace


bool init() {
    if (g_active) return true;
    QueryPerformanceFrequency(&g_freq);
    // openvr_api.dll is delay-loaded; make sure it's there before touching any OpenVR symbol.
    if (!LoadLibraryW(L"openvr_api.dll")) {
        BWLOG("openvr: openvr_api.dll not found next to the game");
        return false;
    }
    if (!::vr::VR_IsRuntimeInstalled()) {
        BWLOG("openvr: SteamVR runtime not installed");
        return false;
    }
    ::vr::EVRInitError err = ::vr::VRInitError_None;
    g_sys = ::vr::VR_Init(&err, ::vr::VRApplication_Scene);
    if (err != ::vr::VRInitError_None) {
        BWLOG("openvr: VR_Init failed: %s", ::vr::VR_GetVRInitErrorAsEnglishDescription(err));
        g_sys = nullptr;
        return false;
    }
    if (!::vr::VRCompositor()) {
        BWLOG("openvr: no compositor");
        ::vr::VR_Shutdown();
        return false;
    }
    ::vr::VRCompositor()->SetTrackingSpace(::vr::TrackingUniverseStanding);
    if (!create_device()) {
        ::vr::VR_Shutdown();
        return false;
    }
    g_last_stats = {};
    ::vr::VRCompositor()->GetCumulativeStats(&g_last_stats, sizeof(g_last_stats));
    g_active = true;
    init_input();
    HmdProfile p = profile();
    BWLOG("openvr: active, %dx%d per eye at %.0f Hz, IPD %.1f mm", p.render_w, p.render_h, p.refresh_hz,
          p.ipd_m * 1000);
    return true;
}

void shutdown() {
    if (!g_active) return;
    for (auto*& t : g_tex)
        if (t) t->Release(), t = nullptr;
    for (SharedEye& s : g_shared) s.release();
    if (g_shared_done) g_shared_done->Release(), g_shared_done = nullptr;
    g_query_dev = nullptr;
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    g_ctx = nullptr;
    g_dev = nullptr;
    ::vr::VR_Shutdown();
    g_sys = nullptr;
    g_active = false;
}

bool active() { return g_active; }

HmdProfile profile() {
    HmdProfile p;
    if (!g_sys) return p;
    uint32_t w = 0, h = 0;
    g_sys->GetRecommendedRenderTargetSize(&w, &h);
    p.render_w = int(w);
    p.render_h = int(h);
    ::vr::ETrackedPropertyError pe;
    p.refresh_hz = g_sys->GetFloatTrackedDeviceProperty(::vr::k_unTrackedDeviceIndex_Hmd,
                                                         ::vr::Prop_DisplayFrequency_Float, &pe);
    p.ipd_m = g_sys->GetFloatTrackedDeviceProperty(::vr::k_unTrackedDeviceIndex_Hmd, ::vr::Prop_UserIpdMeters_Float, &pe);
    char model[64] = {};
    g_sys->GetStringTrackedDeviceProperty(::vr::k_unTrackedDeviceIndex_Hmd, ::vr::Prop_ModelNumber_String, model,
                                          sizeof(model));
    p.model = model;
    for (int e = 0; e < 2; ++e) {
        const ::vr::EVREye eye = e ? ::vr::Eye_Right : ::vr::Eye_Left;
        EyeOptics& o = p.eye[e];
        g_sys->GetProjectionRaw(eye, &o.tan_left, &o.tan_right, &o.tan_top, &o.tan_bottom);
        const ::vr::HmdMatrix34_t m = g_sys->GetEyeToHeadTransform(eye);
        o.offset = to_engine(m.m[0][3], m.m[1][3], m.m[2][3]);
    }
    return p;
}

bool wait_frame(Pose& head) {
    if (!g_active) return false;
    ::vr::TrackedDevicePose_t poses[::vr::k_unMaxTrackedDeviceCount];
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    ::vr::EVRCompositorError e = ::vr::VRCompositor()->WaitGetPoses(poses, ::vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    g_wait_ms += ms_since(t0);
    ++g_waits;
    g_eye_ready[0] = g_eye_ready[1] = false;
    const ::vr::TrackedDevicePose_t& hmd = poses[::vr::k_unTrackedDeviceIndex_Hmd];
    g_last_tracking_result = int(hmd.eTrackingResult);
    g_hmd_connected = hmd.bDeviceIsConnected;
    if (e != ::vr::VRCompositorError_None || !hmd.bPoseIsValid) {
        ++g_invalid_poses;
        return false;
    }
    head = pose_from(hmd.mDeviceToAbsoluteTracking);
    return true;
}

void finish_eye(int eye);  // (both eyes in: submit them)

bool share_eye(int eye, void* device9, void* surface9, int x, int y, int w, int h) {
    if (!g_active || !ensure_texture(eye, w, h)) return false;
    auto* dev9 = static_cast<IDirect3DDevice9*>(device9);
    SharedEye& s = g_shared[eye];
    if (s.dev9 != dev9 || s.w != w || s.h != h) {
        s.release();
        HANDLE handle = nullptr;
        HRESULT hr = dev9->CreateTexture(UINT(w), UINT(h), 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s.tex9, &handle);
        if (FAILED(hr) || !handle) {
            static int logged = 0;
            if (logged++ < 3) BWLOG("openvr: shared D3D9 texture %dx%d failed %08lX (the device must be Direct3D 9Ex: dxwrapper D3d9to9Ex=1)", w, h, hr);
            s.release();
            return false;
        }
        s.tex9->GetSurfaceLevel(0, &s.surf9);
        hr = g_dev->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&s.tex11));
        if (FAILED(hr) || !s.surf9) {
            BWLOG("openvr: opening the shared texture in D3D11 failed %08lX", hr);
            s.release();
            return false;
        }
        s.dev9 = dev9, s.w = w, s.h = h;
        BWLOG("openvr: eye %d shared with Direct3D 9 (%dx%d): no readback", eye, w, h);
    }
    if (g_query_dev != dev9) {
        if (g_shared_done) g_shared_done->Release(), g_shared_done = nullptr;
        dev9->CreateQuery(D3DQUERYTYPE_EVENT, &g_shared_done);
        g_query_dev = dev9;
    }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const RECT r{x, y, x + w, y + h};
    const HRESULT hr = dev9->StretchRect(static_cast<IDirect3DSurface9*>(surface9), &r, s.surf9, nullptr, D3DTEXF_NONE);
    if (FAILED(hr)) {
        static int logged = 0;
        if (logged++ < 3) BWLOG("openvr: StretchRect of eye %d failed %08lX", eye, hr);
        return false;
    }
    g_share_ms += ms_since(t0);
    if (eye == 0) {  // (the left eye waits for the right)
        g_shared_left = true;
        return true;
    }
    // Both copied: Direct3D 9 must finish them before Direct3D 11 reads them.
    QueryPerformanceCounter(&t0);
    if (g_shared_done) {
        g_shared_done->Issue(D3DISSUE_END);
        while (g_shared_done->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) YieldProcessor();
    }
    if (g_shared_left) g_ctx->CopyResource(g_tex[0], g_shared[0].tex11);
    g_ctx->CopyResource(g_tex[1], g_shared[1].tex11);
    g_share_ms += ms_since(t0);
    ++g_shared_frames;
    if (g_shared_left) finish_eye(0);
    g_shared_left = false;
    finish_eye(1);
    return true;
}
bool map_eye(int eye, int w, int h, uint8_t*& dst, int& pitch) {
    if (!g_active || !ensure_texture(eye, w, h)) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(g_upload[eye], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    dst = static_cast<uint8_t*>(m.pData);
    pitch = int(m.RowPitch);
    return true;
}

void submit_mapped_eye(int eye) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    g_ctx->Unmap(g_upload[eye], 0);
    g_ctx->CopyResource(g_tex[eye], g_upload[eye]);
    g_upload_ms += ms_since(t0);
    finish_eye(eye);
}

void submit_eye(int eye, const uint8_t* bgrx, int w, int h, int pitch) {
    if (!g_active || !ensure_texture(eye, w, h)) return;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    g_ctx->UpdateSubresource(g_tex[eye], 0, nullptr, bgrx, UINT(pitch), 0);
    g_upload_ms += ms_since(t0);
    finish_eye(eye);
}

void finish_eye(int eye) {
    g_eye_ready[eye] = true;
    if (!(g_eye_ready[0] && g_eye_ready[1])) return;
    for (int i = 0; i < 2; ++i) {
        ::vr::Texture_t tex{g_tex[i], ::vr::TextureType_DirectX, ::vr::ColorSpace_Gamma};
        ::vr::EVRCompositorError err = ::vr::VRCompositor()->Submit(i ? ::vr::Eye_Right : ::vr::Eye_Left, &tex);
        if (err != ::vr::VRCompositorError_None) {
            static unsigned logged = 0;
            if (logged++ < 5) BWLOG("openvr: Submit(eye %d) error %d", i, int(err));
        }
    }
    ::vr::VRCompositor()->PostPresentHandoff();
    g_eye_ready[0] = g_eye_ready[1] = false;
}

bool read_mirror(int eye, std::vector<uint8_t>& out, int& w, int& h) {
    if (!g_active) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    if (::vr::VRCompositor()->GetMirrorTextureD3D11(eye ? ::vr::Eye_Right : ::vr::Eye_Left, g_dev,
                                                    reinterpret_cast<void**>(&srv)) != ::vr::VRCompositorError_None ||
        !srv)
        return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    bool ok = false;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &staging))) {
            g_ctx->CopyResource(staging, tex);
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                w = int(d.Width);
                h = int(d.Height);
                out.resize(size_t(w) * h * 4);
                const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                                  d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;  // SteamVR's mirror (27)
                for (int y = 0; y < h; ++y) {
                    const uint8_t* src = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
                    uint8_t* dst = &out[size_t(y) * w * 4];
                    memcpy(dst, src, size_t(w) * 4);
                    if (rgba)
                        for (int x = 0; x < w; ++x) std::swap(dst[x * 4], dst[x * 4 + 2]);
                }
                g_ctx->Unmap(staging, 0);
                ok = true;
                static bool logged = false;
                if (!logged) BWLOG("openvr: mirror texture %dx%d format %d", w, h, int(d.Format));
                logged = true;
            }
            staging->Release();
        }
        tex->Release();
    }
    if (res) res->Release();
    ::vr::VRCompositor()->ReleaseMirrorTextureD3D11(srv);
    return ok;
}

void read_controllers(Controller out[2]) {
    for (int h = 0; h < 2; ++h) out[h] = Controller{};
    if (!g_active || !g_input) return;
    ::vr::IVRInput* in = ::vr::VRInput();
    ::vr::VRActiveActionSet_t set{};
    set.ulActionSet = g_set;
    set.ulRestrictedToDevice = ::vr::k_ulInvalidInputValueHandle;
    if (in->UpdateActionState(&set, sizeof(set), 1) != ::vr::VRInputError_None) return;
    for (int h = 0; h < 2; ++h) {
        Controller& c = out[h];
        const auto src = g_hand_src[h];
        ::vr::InputPoseActionData_t pd{};
        if (in->GetPoseActionDataForNextFrame(g_aim, ::vr::TrackingUniverseStanding, &pd, sizeof(pd), src) ==
                ::vr::VRInputError_None && pd.bActive && pd.pose.bPoseIsValid) {
            c.active = true;
            c.aim = pose_from(pd.pose.mDeviceToAbsoluteTracking);
        }
        if (in->GetPoseActionDataForNextFrame(g_grip_pose, ::vr::TrackingUniverseStanding, &pd, sizeof(pd), src) ==
                ::vr::VRInputError_None && pd.bActive && pd.pose.bPoseIsValid) {
            c.grip = pose_from(pd.pose.mDeviceToAbsoluteTracking);
            c.grip_vel = to_engine(pd.pose.vVelocity.v[0], pd.pose.vVelocity.v[1], pd.pose.vVelocity.v[2]);  // (as the positions: z flipped)
            c.has_vel = true;
        } else
            c.grip = c.aim;
        ::vr::InputAnalogActionData_t ad{};
        if (in->GetAnalogActionData(g_trigger, &ad, sizeof(ad), src) == ::vr::VRInputError_None) c.trigger = ad.x;
        if (in->GetAnalogActionData(g_squeeze, &ad, sizeof(ad), src) == ::vr::VRInputError_None) c.squeeze = ad.x;
        if (in->GetAnalogActionData(g_stick, &ad, sizeof(ad), src) == ::vr::VRInputError_None)
            c.stick_x = ad.x, c.stick_y = ad.y;
        ::vr::InputDigitalActionData_t dd{};
        if (in->GetDigitalActionData(g_a, &dd, sizeof(dd), src) == ::vr::VRInputError_None) c.a = dd.bState;
        if (in->GetDigitalActionData(g_b, &dd, sizeof(dd), src) == ::vr::VRInputError_None) c.b = dd.bState;
        if (in->GetDigitalActionData(g_stick_click, &dd, sizeof(dd), src) == ::vr::VRInputError_None)
            c.stick_click = dd.bState;
        if (g_squeeze_click && in->GetDigitalActionData(g_squeeze_click, &dd, sizeof(dd), src) == ::vr::VRInputError_None && dd.bState)
            c.squeeze = 1.0f;
        ::vr::VRSkeletalSummaryData_t sum{};
        if (in->GetSkeletalSummaryData(g_skeleton[h], ::vr::VRSummaryType_FromDevice, &sum) == ::vr::VRInputError_None)
            for (int f = 0; f < 5; ++f) c.curl[f] = sum.flFingerCurl[f];
    }
}

void haptic(int hand, float seconds, float frequency, float amplitude) {
    if (!g_active || !g_input || hand < 0 || hand > 1) return;
    ::vr::VRInput()->TriggerHapticVibrationAction(g_haptic, 0, seconds, frequency, amplitude, g_hand_src[hand]);
}

std::string stats_line() {
    if (!g_active) return "openvr inactive";
    ::vr::Compositor_CumulativeStats s{};
    ::vr::VRCompositor()->GetCumulativeStats(&s, sizeof(s));
    ::vr::Compositor_FrameTiming ft{};
    ft.m_nSize = sizeof(ft);
    ::vr::VRCompositor()->GetFrameTiming(&ft, 0);
    char buf[320];
    const unsigned presents = s.m_nNumFramePresents - g_last_stats.m_nNumFramePresents;
    snprintf(buf, sizeof(buf),
             "openvr: %u presents, %u dropped, %u reprojected, %u timed out | waits %u (avg %.2f ms), invalid poses %u, "
             "upload avg %.2f ms | last frame gpu %.2f ms, compositor gpu %.2f ms",
             presents, s.m_nNumDroppedFrames - g_last_stats.m_nNumDroppedFrames,
             s.m_nNumReprojectedFrames - g_last_stats.m_nNumReprojectedFrames,
             s.m_nNumTimedOut - g_last_stats.m_nNumTimedOut, g_waits, g_waits ? g_wait_ms / g_waits : 0.0,
             g_invalid_poses, g_waits ? g_upload_ms / g_waits : 0.0, ft.m_flTotalRenderGpuMs,
             ft.m_flCompositorRenderGpuMs);
    unsigned base_stations = 0, controllers = 0;
    for (::vr::TrackedDeviceIndex_t d = 0; d < ::vr::k_unMaxTrackedDeviceCount; ++d) {
        if (!g_sys->IsTrackedDeviceConnected(d)) continue;
        const auto cls = g_sys->GetTrackedDeviceClass(d);
        base_stations += cls == ::vr::TrackedDeviceClass_TrackingReference;
        controllers += cls == ::vr::TrackedDeviceClass_Controller;
    }
    const auto level = g_sys->GetTrackedDeviceActivityLevel(::vr::k_unTrackedDeviceIndex_Hmd);
    char extra[160];
    snprintf(extra, sizeof(extra), " | hmd connected %d, tracking result %d, activity %d, base stations %u, controllers %u",
             g_hmd_connected, g_last_tracking_result, int(level), base_stations, controllers);
    std::string shared;
    if (g_shared_frames) {
        char sh[96];
        snprintf(sh, sizeof(sh), " | shared with Direct3D 9: %u frames, %.2f ms each", g_shared_frames, g_share_ms / g_shared_frames);
        shared = sh;
    }
    g_last_stats = s;
    g_waits = g_invalid_poses = 0;
    g_wait_ms = g_upload_ms = 0;
    g_share_ms = 0, g_shared_frames = 0;
    return std::string(buf) + extra + shared;
}

// ---- SteamVR's keyboard (typing a save's name) ----
// Minimal mode: no text field of its own; each key press comes as an event (VREvent_KeyboardCharInput,
// up to 7 bytes of UTF-8; Backspace as the character 8), and the game's own field shows the text. (In the
// buffered mode the player's key presses showed nothing in the keyboard's field, 2026-10-19.)
namespace {
bool g_keyboard_shown = false;
int g_keyboard_events_logged = 0;
}

bool show_keyboard(const char* description, const char* existing, uint32_t max_chars) {
    if (!g_active || !::vr::VROverlay()) return false;
    const ::vr::EVROverlayError e = ::vr::VROverlay()->ShowKeyboard(
        ::vr::k_EGamepadTextInputModeNormal, ::vr::k_EGamepadTextInputLineModeSingleLine,
        ::vr::KeyboardFlag_Minimal | ::vr::KeyboardFlag_Modal, description, max_chars, existing, 0);
    g_keyboard_shown = e == ::vr::VROverlayError_None;
    g_keyboard_events_logged = 0;
    BWLOG("openvr: keyboard %s (%d)", g_keyboard_shown ? "shown" : "could not be shown", int(e));
    return g_keyboard_shown;
}

int poll_keyboard(std::string& typed) {
    if (!g_active || !g_keyboard_shown) return -1;
    ::vr::VREvent_t ev;
    int result = 0;
    while (g_sys->PollNextEvent(&ev, sizeof(ev))) {
        if (g_keyboard_events_logged < 40) {  // (what reaches us while it is up, for the player's sessions)
            ++g_keyboard_events_logged;
            BWLOG("openvr: event %u (%s) while the keyboard is up", ev.eventType,
                  g_sys->GetEventTypeNameFromEnum(::vr::EVREventType(ev.eventType)));
        }
        if (ev.eventType == ::vr::VREvent_KeyboardCharInput) {
            char c[9] = {};
            memcpy(c, ev.data.keyboard.cNewInput, 8);
            typed += c;
        } else if (ev.eventType == ::vr::VREvent_KeyboardDone) {
            result = 1;
        } else if ((ev.eventType == ::vr::VREvent_KeyboardClosed || ev.eventType == ::vr::VREvent_KeyboardClosed_Global) && result == 0) {
            result = -1;
        }
    }
    if (result) g_keyboard_shown = false;
    return result;
}

void hide_keyboard() {
    if (g_active && g_keyboard_shown && ::vr::VROverlay()) ::vr::VROverlay()->HideKeyboard();
    g_keyboard_shown = false;
}

namespace {
::vr::VROverlayHandle_t g_video = ::vr::k_ulOverlayHandleInvalid;
bool g_video_shown = false;
// The picture goes up as a texture (SetOverlayRaw, made for pictures that seldom change, flickered
// at 45 a second; player, 2026-11-02): written into a dynamic texture, copied into one of two
// that take turns, so the compositor never reads the one being written.
ID3D11Texture2D* g_video_upload = nullptr;
ID3D11Texture2D* g_video_tex[2] = {};
int g_video_w = 0, g_video_h = 0, g_video_next = 0;
bool video_textures(int w, int h) {
    if (!g_dev) return false;
    if (g_video_upload && w == g_video_w && h == g_video_h) return true;
    if (g_video_upload) g_video_upload->Release(), g_video_upload = nullptr;
    for (auto*& t : g_video_tex)
        if (t) t->Release(), t = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w), d.Height = UINT(h), d.MipLevels = 1, d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (auto*& t : g_video_tex)
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) return false;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_video_upload))) return false;
    g_video_w = w, g_video_h = h;
    return true;
}
}

bool show_video(const uint8_t* rgba, int w, int h, float width_m, float distance_m) {
    if (!g_active || !::vr::VROverlay() || !rgba || w <= 0 || h <= 0) return false;
    ::vr::IVROverlay* ov = ::vr::VROverlay();
    if (g_video == ::vr::k_ulOverlayHandleInvalid) {
        const ::vr::EVROverlayError e = ov->CreateOverlay("bwvr.video", "Black & White video", &g_video);
        if (e != ::vr::VROverlayError_None) {
            BWLOG("openvr: the video screen overlay could not be made (%d)", int(e));
            g_video = ::vr::k_ulOverlayHandleInvalid;
            return false;
        }
    }
    if (!g_video_shown) {
        // Ahead of the head as it is now, level, facing it (OpenVR axes: -z is forward, y up).
        ::vr::TrackedDevicePose_t pose{};
        g_sys->GetDeviceToAbsoluteTrackingPose(::vr::TrackingUniverseStanding, 0, &pose, 1);
        const ::vr::HmdMatrix34_t& m = pose.mDeviceToAbsoluteTracking;
        float px = m.m[0][3], py = m.m[1][3], pz = m.m[2][3];
        float fx = -m.m[0][2], fz = -m.m[2][2];
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (!pose.bPoseIsValid || fl < 1e-3f) px = 0, py = 1.6f, pz = 0, fx = 0, fz = -1;
        else fx /= fl, fz /= fl;
        // The overlay is seen from its +z side: +z toward the head, x = y cross z.
        const float zx = -fx, zz = -fz;
        const float xx = zz, xz = -zx;
        ::vr::HmdMatrix34_t t{};
        t.m[0][0] = xx, t.m[0][1] = 0, t.m[0][2] = zx, t.m[0][3] = px + fx * distance_m;
        t.m[1][0] = 0, t.m[1][1] = 1, t.m[1][2] = 0, t.m[1][3] = py;
        t.m[2][0] = xz, t.m[2][1] = 0, t.m[2][2] = zz, t.m[2][3] = pz + fz * distance_m;
        ov->SetOverlayTransformAbsolute(g_video, ::vr::TrackingUniverseStanding, &t);
        ov->SetOverlayWidthInMeters(g_video, width_m);
        ov->ShowOverlay(g_video);
        g_video_shown = true;
        BWLOG("openvr: the video screen shown (%dx%d), %.1f m wide %.1f m ahead", w, h, width_m, distance_m);
    }
    if (!video_textures(w, h)) {
        const ::vr::EVROverlayError e = ov->SetOverlayRaw(g_video, const_cast<uint8_t*>(rgba), uint32_t(w), uint32_t(h), 4);
        return e == ::vr::VROverlayError_None;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(g_video_upload, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    for (int y = 0; y < h; ++y) memcpy(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, rgba + size_t(y) * w * 4, size_t(w) * 4);
    g_ctx->Unmap(g_video_upload, 0);
    ID3D11Texture2D* t = g_video_tex[g_video_next];
    g_video_next ^= 1;
    g_ctx->CopyResource(t, g_video_upload);
    g_ctx->Flush();
    ::vr::Texture_t tex{t, ::vr::TextureType_DirectX, ::vr::ColorSpace_Auto};
    return ov->SetOverlayTexture(g_video, &tex) == ::vr::VROverlayError_None;
}

namespace {
::vr::VROverlayHandle_t g_saving = ::vr::k_ulOverlayHandleInvalid;
ULONGLONG g_saving_shown_at = 0;
bool g_saving_up = false, g_saving_hide = false;
// "Saving..." in white on a soft dark panel, drawn once with GDI (RGBA, top-down).
std::vector<uint8_t> saving_picture(int w, int h) {
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w, bi.bmiHeader.biHeight = -h, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return rgba;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    memset(bits, 0, size_t(w) * h * 4);
    HFONT font = CreateFontW(-h * 11 / 20, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Georgia");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r{0, 0, w, h};
    DrawTextW(dc, L"Saving\x2026", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // (The panel: dark, rounder at its ends by fading out over the last eighth of its width.)
            const float edge = std::min(1.0f, std::min(float(x), float(w - 1 - x)) / (w / 8.0f));
            const float text = src[(size_t(y) * w + x) * 4 + 1] / 255.0f;
            const float bg = 0.55f * edge;
            const float a = std::max(text, bg);
            const float c = a > 0 ? text / a : 0;  // (the colour: white where the words are, dark brown elsewhere)
            uint8_t* d = &rgba[(size_t(y) * w + x) * 4];
            d[0] = uint8_t(20 + c * 235), d[1] = uint8_t(16 + c * 230), d[2] = uint8_t(10 + c * 210), d[3] = uint8_t(a * 255.0f);
        }
    SelectObject(dc, old_font);
    DeleteObject(font);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return rgba;
}
}

void saving_cover(bool on, float alpha, float fade_s) {
    if (!g_active || !::vr::VROverlay() || !::vr::VRCompositor()) return;
    ::vr::IVROverlay* ov = ::vr::VROverlay();
    if (on && g_saving_up && !g_saving_hide) return;  // (already up: faded out before the save began)
    if (on) {
        if (g_saving == ::vr::k_ulOverlayHandleInvalid) {
            if (ov->CreateOverlay("bwvr.saving", "Black & White saving", &g_saving) != ::vr::VROverlayError_None) {
                g_saving = ::vr::k_ulOverlayHandleInvalid;
                return;
            }
            const int w = 512, h = 128;
            std::vector<uint8_t> pic = saving_picture(w, h);
            ov->SetOverlayRaw(g_saving, pic.data(), uint32_t(w), uint32_t(h), 4);
            ov->SetOverlayWidthInMeters(g_saving, 0.5f);
            ov->SetOverlaySortOrder(g_saving, 10);
        }
        // Ahead of the head as it is now, a little below the eyes, level, facing it.
        ::vr::TrackedDevicePose_t pose{};
        g_sys->GetDeviceToAbsoluteTrackingPose(::vr::TrackingUniverseStanding, 0, &pose, 1);
        const ::vr::HmdMatrix34_t& m = pose.mDeviceToAbsoluteTracking;
        float px = m.m[0][3], py = m.m[1][3], pz = m.m[2][3];
        float fx = -m.m[0][2], fz = -m.m[2][2];
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (!pose.bPoseIsValid || fl < 1e-3f) px = 0, py = 1.6f, pz = 0, fx = 0, fz = -1;
        else fx /= fl, fz /= fl;
        const float d = 1.5f, zx = -fx, zz = -fz, xx = zz, xz = -zx;
        ::vr::HmdMatrix34_t t{};
        t.m[0][0] = xx, t.m[0][2] = zx, t.m[0][3] = px + fx * d;
        t.m[1][1] = 1, t.m[1][3] = py;  // (in the middle of the view: player, 2026-11-05)
        t.m[2][0] = xz, t.m[2][2] = zz, t.m[2][3] = pz + fz * d;
        ov->SetOverlayTransformAbsolute(g_saving, ::vr::TrackingUniverseStanding, &t);
        ov->ShowOverlay(g_saving);
        g_saving_shown_at = GetTickCount64(), g_saving_up = true, g_saving_hide = false;
        // The view faded (the compositor fades it, going on while the game is held up).
        ::vr::VRCompositor()->FadeToColor(fade_s, 0, 0, 0, alpha, false);
        BWLOG("openvr: saving: the view faded to %.0f%% black over %.2f s, \"Saving...\" shown", alpha * 100.0f, fade_s);
    } else {
        ::vr::VRCompositor()->FadeToColor(0.3f, 0, 0, 0, 0, false);
        g_saving_hide = true;  // (the panel itself after a second up: saving_tick)
        saving_tick();
        BWLOG("openvr: saving: done, the view back");
    }
}

void saving_tick() {
    if (!g_saving_up || !g_saving_hide || GetTickCount64() - g_saving_shown_at < 1000) return;
    if (g_active && ::vr::VROverlay() && g_saving != ::vr::k_ulOverlayHandleInvalid) ::vr::VROverlay()->HideOverlay(g_saving);
    g_saving_up = g_saving_hide = false;
}

void hide_video() {
    if (!g_video_shown) return;
    if (g_active && ::vr::VROverlay()) ::vr::VROverlay()->HideOverlay(g_video);
    g_video_shown = false;
    BWLOG("openvr: the video screen hidden");
}

}  // namespace bw::vr::openvr
