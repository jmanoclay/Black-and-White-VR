// Records the connected HMD's optics so the MockBackend can reproduce them without a headset.
// Usage: bwvr_probe.exe <output.json>
#include <openvr.h>

#include <cstdio>
#include <string>

static std::string prop_string(vr::IVRSystem* sys, vr::ETrackedDeviceProperty p) {
    char buf[256] = {};
    sys->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, p, buf, sizeof(buf));
    return buf;
}

static void write_matrix34(FILE* f, const vr::HmdMatrix34_t& m) {
    fprintf(f, "[");
    for (int r = 0; r < 3; ++r)
        fprintf(f, "%s[%.9g, %.9g, %.9g, %.9g]", r ? ", " : "", m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3]);
    fprintf(f, "]");
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "hmd_profile.json";
    vr::EVRInitError err = vr::VRInitError_None;
    // Background first, so an already-running SteamVR isn't disturbed; otherwise start it.
    vr::IVRSystem* sys = vr::VR_Init(&err, vr::VRApplication_Background);
    if (err == vr::VRInitError_Init_NoServerForBackgroundApp) sys = vr::VR_Init(&err, vr::VRApplication_Other);
    if (err != vr::VRInitError_None) {
        fprintf(stderr, "VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    if (!sys->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd)) {
        fprintf(stderr, "no HMD connected\n");
        vr::VR_Shutdown();
        return 2;
    }

    FILE* f = fopen(out, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", out);
        vr::VR_Shutdown();
        return 3;
    }
    uint32_t w = 0, h = 0;
    sys->GetRecommendedRenderTargetSize(&w, &h);
    vr::ETrackedPropertyError pe;
    float hz = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &pe);
    float ipd = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &pe);

    fprintf(f, "{\n  \"source\": \"bwvr_probe\",\n");
    fprintf(f, "  \"model\": \"%s\",\n  \"manufacturer\": \"%s\",\n", prop_string(sys, vr::Prop_ModelNumber_String).c_str(),
            prop_string(sys, vr::Prop_ManufacturerName_String).c_str());
    fprintf(f, "  \"tracking_system\": \"%s\",\n", prop_string(sys, vr::Prop_TrackingSystemName_String).c_str());
    fprintf(f, "  \"recommended_render_size\": [%u, %u],\n  \"refresh_hz\": %.3f,\n  \"ipd_m\": %.5f,\n", w, h, hz, ipd);
    fprintf(f, "  \"eyes\": {\n");
    const vr::EVREye eyes[] = {vr::Eye_Left, vr::Eye_Right};
    for (int i = 0; i < 2; ++i) {
        float l, r, t, b;
        sys->GetProjectionRaw(eyes[i], &l, &r, &t, &b);
        fprintf(f, "    \"%s\": {\n", i ? "right" : "left");
        fprintf(f, "      \"projection_raw\": {\"left\": %.9g, \"right\": %.9g, \"top\": %.9g, \"bottom\": %.9g},\n", l, r, t, b);
        fprintf(f, "      \"eye_to_head\": ");
        write_matrix34(f, sys->GetEyeToHeadTransform(eyes[i]));
        vr::HiddenAreaMesh_t mesh = sys->GetHiddenAreaMesh(eyes[i], vr::k_eHiddenAreaMesh_Standard);
        fprintf(f, ",\n      \"hidden_area_triangles\": [");
        for (uint32_t v = 0; v < mesh.unTriangleCount * 3; ++v)
            fprintf(f, "%s[%.6g, %.6g]", v ? ", " : "", mesh.pVertexData[v].v[0], mesh.pVertexData[v].v[1]);
        fprintf(f, "]\n    }%s\n", i ? "" : ",");
    }
    fprintf(f, "  },\n  \"devices\": [");
    bool first = true;
    for (vr::TrackedDeviceIndex_t d = 0; d < vr::k_unMaxTrackedDeviceCount; ++d) {
        if (!sys->IsTrackedDeviceConnected(d)) continue;
        char model[128] = {};
        sys->GetStringTrackedDeviceProperty(d, vr::Prop_ModelNumber_String, model, sizeof(model));
        fprintf(f, "%s{\"index\": %u, \"class\": %d, \"model\": \"%s\"}", first ? "" : ", ", d,
                int(sys->GetTrackedDeviceClass(d)), model);
        first = false;
    }
    fprintf(f, "]\n}\n");
    fclose(f);
    printf("wrote %s: %s, %ux%u per eye, %.0f Hz, IPD %.1f mm\n", out, prop_string(sys, vr::Prop_ModelNumber_String).c_str(),
           w, h, hz, ipd * 1000.0f);
    vr::VR_Shutdown();
    return 0;
}
