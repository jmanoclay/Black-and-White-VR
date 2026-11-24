#pragma once
// Sets Direct3D 7 states for one of the mod's own draws and puts back exactly those it changed
// when it goes out of scope. (Direct3D 7 state blocks did this before, but through dxwrapper an
// applied state block didn't bring the texture stages back: after the hand was drawn, the game drew
// the whole world without its textures.) Include after <d3d.h>, with CINTERFACE.

#include <vector>

namespace bw::render {

class StateSaver {
public:
    explicit StateSaver(IDirect3DDevice7* dev) : dev_(dev) {}
    StateSaver(const StateSaver&) = delete;
    StateSaver& operator=(const StateSaver&) = delete;

    void rs(D3DRENDERSTATETYPE type, DWORD value) {
        DWORD old = 0;
        if (SUCCEEDED(dev_->lpVtbl->GetRenderState(dev_, type, &old))) rs_.push_back({DWORD(type), old});
        dev_->lpVtbl->SetRenderState(dev_, type, value);
    }
    void tss(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
        DWORD old = 0;
        if (SUCCEEDED(dev_->lpVtbl->GetTextureStageState(dev_, stage, type, &old))) tss_.push_back({stage, DWORD(type), old});
        dev_->lpVtbl->SetTextureStageState(dev_, stage, type, value);
    }
    void texture(DWORD stage, IDirectDrawSurface7* surface) {
        IDirectDrawSurface7* old = nullptr;
        if (SUCCEEDED(dev_->lpVtbl->GetTexture(dev_, stage, &old))) tex_.push_back({stage, old});  // (AddRef'd: released after it's put back)
        dev_->lpVtbl->SetTexture(dev_, stage, surface);
    }
    ~StateSaver() {
        for (auto it = tex_.rbegin(); it != tex_.rend(); ++it) {
            dev_->lpVtbl->SetTexture(dev_, it->stage, it->surface);
            if (it->surface) it->surface->lpVtbl->Release(it->surface);
        }
        for (auto it = tss_.rbegin(); it != tss_.rend(); ++it)
            dev_->lpVtbl->SetTextureStageState(dev_, it->stage, D3DTEXTURESTAGESTATETYPE(it->type), it->value);
        for (auto it = rs_.rbegin(); it != rs_.rend(); ++it) dev_->lpVtbl->SetRenderState(dev_, D3DRENDERSTATETYPE(it->type), it->value);
    }

private:
    struct Rs {
        DWORD type, value;
    };
    struct Tss {
        DWORD stage, type, value;
    };
    struct Tex {
        DWORD stage;
        IDirectDrawSurface7* surface;
    };
    IDirect3DDevice7* dev_;
    std::vector<Rs> rs_;
    std::vector<Tss> tss_;
    std::vector<Tex> tex_;
};

}  // namespace bw::render
