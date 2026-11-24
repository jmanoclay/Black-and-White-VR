#include "game/camdump.h"

#include <cstdio>

#include "game/engine.h"

namespace bw::game {

namespace {

void write_point(FILE* f, const char* key, const LHPoint& p, bool comma = true) {
    fprintf(f, "  \"%s\": [%.6f, %.6f, %.6f]%s\n", key, p.x, p.y, p.z, comma ? "," : "");
}

void write_matrix(FILE* f, const char* key, const LHMatrix& m) {
    fprintf(f, "  \"%s\": [", key);
    for (int r = 0; r < 4; ++r)
        fprintf(f, "%s[%.7f, %.7f, %.7f]", r ? ", " : "", m.m[r][0], m.m[r][1], m.m[r][2]);
    fprintf(f, "],\n");
}

}  // namespace

void dump_camera(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    const LH3DCamera& c = camera();
    const InfoTransform& t = info_transform();
    fprintf(f, "{\n");
    write_point(f, "pos", c.pos);
    write_point(f, "foc", c.foc);
    write_point(f, "look_at", c.look_at);
    fprintf(f, "  \"fov\": %.7f,\n", c.fov);
    write_matrix(f, "world_to_camera", world_to_camera());
    write_matrix(f, "world_to_clipping", world_to_clipping());
    fprintf(f, "  \"info\": {\"near_clip\": %.6f, \"resolution\": [%d, %d], \"aspect\": %.7f, \"half_res\": [%.3f, %.3f], "
               "\"inv_half_res\": [%.7f, %.7f], \"inv_half_tan_fov_y\": %.7f, \"inv_half_tan_fov_x\": %.7f, "
               "\"cos_half_fov_sqr\": %.7f, \"f2c\": %.7f, \"cos_half_fov\": %.7f, \"f34\": %.7f, \"f38\": %.7f, \"f3c\": %.7f},\n",
            t.near_clip, t.resolution.x, t.resolution.y, t.aspect_x_over_y, t.half_res[0], t.half_res[1],
            t.inv_half_res[0], t.inv_half_res[1], t.inv_half_tan_fov_y, t.inv_half_tan_fov_x, t.cos_half_fov_sqr,
            t.field_2c, t.cos_half_fov, t.field_34, t.inv_aspect_sqr_hypo_inv_times_inv_aspect,
            t.inv_aspect_sqr_hypo_inv);
    fprintf(f, "  \"meter_on_2\": [%.7f, %.7f],\n", meter_width_on_2(), meter_height_on_2());

    // Probe points: the focus, offsets along each world axis, and points along the view ray.
    fprintf(f, "  \"probes\": [\n");
    const float offsets[] = {0.f, 5.f, -5.f, 20.f, -20.f};
    bool first = true;
    for (int axis = 0; axis < 3; ++axis) {
        for (float o : offsets) {
            if (axis > 0 && o == 0.f) continue;
            LHPoint p = c.foc;
            (&p.x)[axis] += o;
            int x = -1, y = -1;
            float depth = -1.f;
            uint32_t vis = project_point(p, &x, &y, &depth);
            fprintf(f, "%s    {\"p\": [%.5f, %.5f, %.5f], \"vis\": %u, \"x\": %d, \"y\": %d, \"depth\": %.6f}",
                    first ? "" : ",\n", p.x, p.y, p.z, vis, x, y, depth);
            first = false;
        }
    }
    fprintf(f, "\n  ]\n}\n");
    fclose(f);
}

}  // namespace bw::game
