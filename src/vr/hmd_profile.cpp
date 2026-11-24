#include "vr/hmd_profile.h"

#include <cstdio>
#include <cstring>

#include "core/log.h"

namespace bw::vr {

HmdProfile load_profile(const std::wstring& path) {
    HmdProfile p;
    p.eye[0].offset = {-0.0315f, 0, 0};
    p.eye[1].offset = {0.0315f, 0, 0};
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) {
        BWLOG("hmd profile %ls not found; using generic optics", path.c_str());
        return p;
    }
    char key[64], sval[128];
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %127s", key, sval) != 2) continue;
        float v = float(atof(sval));
        if (!strcmp(key, "model")) p.model = sval;
        else if (!strcmp(key, "render_w")) p.render_w = int(v);
        else if (!strcmp(key, "render_h")) p.render_h = int(v);
        else if (!strcmp(key, "refresh_hz")) p.refresh_hz = v;
        else if (!strcmp(key, "ipd_m")) p.ipd_m = v;
        else {
            // left.tan_left, right.offset_x, ...
            int e = !strncmp(key, "left.", 5) ? 0 : !strncmp(key, "right.", 6) ? 1 : -1;
            if (e < 0) continue;
            const char* k = key + (e ? 6 : 5);
            EyeOptics& o = p.eye[e];
            if (!strcmp(k, "tan_left")) o.tan_left = v;
            else if (!strcmp(k, "tan_right")) o.tan_right = v;
            else if (!strcmp(k, "tan_top")) o.tan_top = v;
            else if (!strcmp(k, "tan_bottom")) o.tan_bottom = v;
            else if (!strcmp(k, "offset_x")) o.offset.x = v;
            else if (!strcmp(k, "offset_y")) o.offset.y = v;
            else if (!strcmp(k, "offset_z")) o.offset.z = v;
        }
    }
    fclose(f);
    BWLOG("hmd profile: %s, %dx%d, %.0f Hz, IPD %.1f mm; L tan [%.3f %.3f %.3f %.3f] R tan [%.3f %.3f %.3f %.3f]",
          p.model.c_str(), p.render_w, p.render_h, p.refresh_hz, p.ipd_m * 1000, p.eye[0].tan_left,
          p.eye[0].tan_right, p.eye[0].tan_top, p.eye[0].tan_bottom, p.eye[1].tan_left, p.eye[1].tan_right,
          p.eye[1].tan_top, p.eye[1].tan_bottom);
    return p;
}

}  // namespace bw::vr
